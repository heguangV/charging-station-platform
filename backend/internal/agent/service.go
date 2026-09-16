package agent

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"
)

// The orchestration core: understand the question, decide which tools to run,
// run them in an order that lets each one use the previous one's findings,
// collect the structured data, and write an answer.
//
// The dependency rule is the important part and it is deliberate: the assistant
// reads the station domain and the map ports, and writes nothing. It cannot
// create an order, move a charger or change a balance, which is what makes it
// safe to expose to a browser and to call with a user's session.
//
// The other rule is that it never fails. A deployment with no model and no map
// key still answers, from deterministic tool selection and fixed wording, and
// says in `degraded` that it did.

// The tool names. They are the frozen identifiers of the contract: they appear
// in the response, in the model's tool list and in the endpoint's schema enum.
const (
	toolStationSearch = "station_search"
	toolStationDetail = "station_detail"
	toolPoiSearch     = "poi_search"
	toolRoute         = "route"
)

// executionOrder fixes the run order regardless of the order the model asked
// in. station_search runs first so that the stations it finds can serve as the
// anchor the later tools need - a route needs a destination coordinate and a
// detail needs a station id, and the model knows neither.
var executionOrder = []string{toolStationSearch, toolStationDetail, toolPoiSearch, toolRoute}

// Config wires the service to its capabilities.
//
// Pois, Routes and LLM may be nil, and a nil capability is treated exactly like
// an unconfigured one: the corresponding tool reports that it cannot look, and
// the answer is degraded rather than broken. Refusing to start without a map
// key would make the assistant impossible to run in development.
type Config struct {
	Stations  StationDirectory
	Pois      PoiProvider
	Routes    RoutePlanner
	LLM       LLMClient
	Converter CoordinateConverter
	Limits    Limits
	Clock     func() time.Time
	Logger    *slog.Logger
}

// Service answers one conversation at a time. It holds no per-conversation
// state, so a single instance serves every request.
type Service struct {
	stations       StationDirectory
	pois           PoiProvider
	routes         RoutePlanner
	llm            LLMClient
	converter      CoordinateConverter
	limits         Limits
	clock          func() time.Time
	logger         *slog.Logger
	tools          []Tool
	toolIndex      map[string]Tool
	budget         time.Duration
	planningBudget time.Duration
}

// NewService validates its inputs and registers the tools.
func NewService(cfg Config) (*Service, error) {
	if cfg.Stations == nil {
		return nil, errors.New("agent: a station directory is required: every answer is grounded in station data")
	}
	limits := cfg.Limits
	defaults := DefaultLimits()
	if limits.MaxToolCalls <= 0 {
		limits.MaxToolCalls = defaults.MaxToolCalls
	}
	if limits.MaxStations <= 0 {
		limits.MaxStations = defaults.MaxStations
	}
	if limits.MaxPois <= 0 {
		limits.MaxPois = defaults.MaxPois
	}
	if limits.DefaultPoiRadiusMeter <= 0 {
		limits.DefaultPoiRadiusMeter = defaults.DefaultPoiRadiusMeter
	}
	if limits.MaxMessageLength <= 0 {
		limits.MaxMessageLength = defaults.MaxMessageLength
	}
	if limits.MaxReplyLength <= 0 {
		limits.MaxReplyLength = defaults.MaxReplyLength
	}
	clock := cfg.Clock
	if clock == nil {
		clock = time.Now
	}
	logger := cfg.Logger
	if logger == nil {
		logger = slog.New(slog.NewTextHandler(io.Discard, nil))
	}

	service := &Service{
		stations:       cfg.Stations,
		pois:           cfg.Pois,
		routes:         cfg.Routes,
		llm:            cfg.LLM,
		converter:      cfg.Converter,
		limits:         limits,
		clock:          clock,
		logger:         logger,
		toolIndex:      map[string]Tool{},
		budget:         defaultChatBudget,
		planningBudget: 4 * time.Second,
	}
	for _, tool := range []Tool{
		NewStationSearchTool(cfg.Stations),
		NewStationDetailTool(cfg.Stations),
		NewPoiSearchTool(cfg.Pois, int64(limits.DefaultPoiRadiusMeter)),
		NewRouteTool(cfg.Routes),
	} {
		service.tools = append(service.tools, tool)
		service.toolIndex[tool.Name()] = tool
	}
	return service, nil
}

// ToolNames lists the registered tools in registration order.
func (s *Service) ToolNames() []string {
	names := make([]string, 0, len(s.tools))
	for _, tool := range s.tools {
		names = append(names, tool.Name())
	}
	return names
}

// PlanTools is the deterministic intent decision.
//
// It exists for two reasons and both matter: it is the plan when no model is
// configured, and it is also the plan when a model answers without asking for
// any tool - a model that only talks would otherwise leave the client with a
// sentence and no data. At least one tool is always returned.
func (s *Service) PlanTools(message string) []string {
	planned := []string{toolStationSearch}
	if containsAny(message, "导航", "怎么走", "路线", "多远", "多久", "开车", "步行", "公交",
		"地铁", "过去", "到那里") {
		planned = append(planned, toolRoute)
	}
	if containsAny(message, "吃", "餐", "饭", "美食", "咖啡", "喝", "便利", "超市", "商场", "购物", "买") {
		planned = append(planned, toolPoiSearch)
	}
	if containsAny(message, "详情", "营业", "几点", "充电桩", "设备", "功率", "接口", "快充桩",
		"慢充桩", "多少桩", "价格构成") {
		planned = append(planned, toolStationDetail)
	}
	return planned
}

// normalizePosition converts a browser position into the platform's datum.
//
// It reports failure rather than returning the original: an uncorrected WGS-84
// position is several hundred metres out, which is the distance between the
// station across the road and the one two streets away, so answering from it
// would answer a different question than the one that was asked.
func (s *Service) normalizePosition(position Location) (Location, bool) {
	if s.converter == nil {
		return Location{}, false
	}
	converted, err := s.converter.ToGCJ02(position)
	if err != nil {
		s.logger.Warn("agent: the browser position could not be converted", "error", err.Error())
		return Location{}, false
	}
	return converted, true
}

// Invocation is one planned tool call.
type Invocation struct {
	Name      string
	Arguments Arguments
}

// planFromModel asks the model which tools to run.
//
// A model that invents a tool name is not an error: the name is dropped, and
// the deterministic anchor below supplies whatever the question genuinely
// needs.
func (s *Service) planFromModel(ctx context.Context, message string, conv Context, reachable *bool) []Invocation {
	response, err := s.llm.Chat(ctx, ChatRequest{
		Messages: []Message{s.systemMessage(), s.userMessage(message, conv)},
		Tools:    s.toolSpecs(),
	})
	if err != nil {
		s.logger.Warn("agent: the model could not plan", "error", err.Error())
		*reachable = false
		return nil
	}
	*reachable = true

	var planned []Invocation
	for _, call := range response.ToolCalls {
		if _, registered := s.toolIndex[call.Name]; !registered {
			continue
		}
		invocation := Invocation{Name: call.Name}
		if strings.TrimSpace(call.ArgumentsJSON) != "" {
			var arguments Arguments
			decoder := json.NewDecoder(strings.NewReader(call.ArgumentsJSON))
			decoder.UseNumber()
			if err := decoder.Decode(&arguments); err == nil {
				invocation.Arguments = arguments
			}
		}
		planned = append(planned, invocation)
	}
	return planned
}

// compose asks the model to write the answer from the observations.
func (s *Service) compose(ctx context.Context, message string, conv Context, observations []string) (string, bool) {
	var data strings.Builder
	data.WriteString(dataInstruction)
	for _, observation := range observations {
		data.WriteString(observation)
		data.WriteString("\n")
	}
	data.WriteString(dataInstructionTail)

	response, err := s.llm.Chat(ctx, ChatRequest{
		Messages: []Message{
			s.systemMessage(),
			s.userMessage(message, conv),
			{Role: RoleUser, Content: data.String()},
		},
	})
	if err != nil {
		s.logger.Warn("agent: the model could not write the reply", "error", err.Error())
		return "", false
	}
	text := trimReply(response.Content, s.limits.MaxReplyLength)
	if text == "" {
		return "", false
	}
	return text, true
}

// withAnchor inserts a station search when the plan needs a value only a search
// can supply.
//
// A model cannot know a station id or a destination coordinate, so a plan that
// goes straight to a detail or a route would fail for a reason that has nothing
// to do with the question. Searching first is what makes those tools work
// without teaching the model the database.
func (s *Service) withAnchor(planned []Invocation, conv Context) []Invocation {
	needsAnchor := false
	hasSearch := false
	for _, invocation := range planned {
		if requiresStationAnchor(invocation.Name, invocation.Arguments, conv) {
			needsAnchor = true
		}
		if invocation.Name == toolStationSearch {
			hasSearch = true
		}
	}
	if !needsAnchor || hasSearch {
		return planned
	}
	return append([]Invocation{{Name: toolStationSearch}}, planned...)
}

// execute runs the plan and collects what it found.
//
// Each tool runs at most once: a model that asks for the same search three
// times would otherwise spend the request budget repeating itself. The run
// stops at the configured tool limit, and the excess is dropped silently
// because a partial answer is still an answer.
func (s *Service) execute(ctx context.Context, planned []Invocation, conv Context, result *Result, degraded *bool) (map[string]bool, []string) {
	ordered := make([]Invocation, 0, len(planned))
	seen := map[string]bool{}
	for _, name := range executionOrder {
		for _, invocation := range planned {
			if invocation.Name != name || seen[name] {
				continue
			}
			seen[name] = true
			ordered = append(ordered, invocation)
		}
	}

	var (
		observations []string
		anchor       *StationSummary
		// failed records which tools could not look, so the wording can tell a
		// search that found nothing from a search that never happened. Saying
		// "no stations nearby" when the database was unreachable would be a
		// confident answer to a question nobody asked.
		failed = map[string]bool{}
	)
	executed := len(ordered)
	if executed > s.limits.MaxToolCalls {
		executed = s.limits.MaxToolCalls
	}
	for index := 0; index < executed; index++ {
		invocation := ordered[index]
		if ctx.Err() != nil {
			failed[invocation.Name] = true
			*degraded = true
			continue
		}
		tool, registered := s.toolIndex[invocation.Name]
		if !registered {
			continue
		}

		arguments := invocation.Arguments
		if anchor != nil {
			arguments = injectAnchor(invocation.Name, arguments, *anchor)
		}

		toolResult := tool.Invoke(ctx, conv, arguments)
		result.Tools = append(result.Tools, invocation.Name)
		if !toolResult.OK {
			// One tool failing degrades this answer and nothing else: the other
			// tools still run, and the internal reason never leaves the process.
			*degraded = true
			failed[invocation.Name] = true
			s.logger.Warn("agent: a tool could not look",
				"tool", invocation.Name, "reason", toolResult.Err)
			observations = append(observations, toolResult.Observation)
			continue
		}
		observations = append(observations, toolResult.Observation)
		s.collect(toolResult, result)
		if invocation.Name == toolStationSearch && len(result.Stations) > 0 {
			nearest := result.Stations[0]
			anchor = &nearest
		}
	}
	return failed, observations
}

// injectAnchor supplies the values a tool cannot know, without overwriting
// anything the model did supply.
func injectAnchor(name string, arguments Arguments, anchor StationSummary) Arguments {
	if arguments == nil {
		arguments = Arguments{}
	}
	switch name {
	case toolPoiSearch:
		if !hasArgument(arguments, "latitudeE6") {
			arguments["latitudeE6"] = anchor.LatitudeE6
			arguments["longitudeE6"] = anchor.LongitudeE6
		}
	case toolRoute:
		if !hasArgument(arguments, "destinationLatitudeE6") {
			arguments["destinationLatitudeE6"] = anchor.LatitudeE6
			arguments["destinationLongitudeE6"] = anchor.LongitudeE6
			arguments["destinationName"] = anchor.Name
		}
	case toolStationDetail:
		if !hasArgument(arguments, "stationId") {
			arguments["stationId"] = anchor.ID
		}
	}
	return arguments
}

// collect merges a tool's findings into the result, deduplicating stations so
// the same one cannot appear twice with different distances.
func (s *Service) collect(toolResult ToolResult, result *Result) {
	for _, station := range toolResult.Stations {
		if len(result.Stations) >= s.limits.MaxStations {
			break
		}
		duplicate := false
		for _, existing := range result.Stations {
			if existing.ID == station.ID {
				duplicate = true
				break
			}
		}
		if !duplicate {
			result.Stations = append(result.Stations, station)
		}
	}
	for _, poi := range toolResult.Pois {
		if len(result.Pois) >= s.limits.MaxPois {
			break
		}
		result.Pois = append(result.Pois, poi)
	}
	if toolResult.Route != nil && result.Route == nil {
		route := *toolResult.Route
		result.Route = &route
	}
}

// buildActions derives the buttons the client offers next.
func (s *Service) buildActions(result *Result) {
	for _, station := range result.Stations {
		result.Actions = append(result.Actions, Action{
			Type:     ActionOpenStation,
			Label:    "查看 " + station.Name,
			TargetID: formatInt64(station.ID),
		})
	}
	if result.Route != nil {
		result.Actions = append(result.Actions, Action{
			Type:  ActionNavigate,
			Label: "导航前往 " + result.Route.DestinationName,
			URL:   result.Route.BrowserURL,
		})
	}
}

// fallbackReply writes the answer without a model.
//
// It reports what was actually found, and distinguishes the two cases a reader
// cares about: nothing matched the filters, versus nothing could be looked at.
func (s *Service) fallbackReply(message string, planned []Invocation, result Result, failed map[string]bool, modelAnswered bool) string {
	var reply strings.Builder
	if !modelAnswered {
		reply.WriteString(llmUnavailableNotice)
	}
	if len(result.Stations) > 0 {
		if reply.Len() > 0 {
			reply.WriteString(" ")
		}
		reply.WriteString("为你找到 ")
		reply.WriteString(formatInt(len(result.Stations)))
		reply.WriteString(" 个充电站：")
		for _, station := range result.Stations {
			reply.WriteString("\n· ")
			reply.WriteString(station.Name)
			reply.WriteString("，")
			if station.HasDistance {
				reply.WriteString(distanceText(station.DistanceMeter))
				reply.WriteString("，")
			}
			reply.WriteString("总价 ")
			reply.WriteString(yuan(station.TotalPriceCentPerKwh))
			reply.WriteString(" 元/千瓦时，空闲 ")
			reply.WriteString(formatInt64(station.IdleChargerCount))
			reply.WriteString("/")
			reply.WriteString(formatInt64(station.ChargerCount))
		}
	} else if failed[toolStationSearch] {
		if reply.Len() > 0 {
			reply.WriteString(" ")
		}
		reply.WriteString(stationQueryFailedNotice)
	} else if len(planned) > 0 && planned[0].Name == toolStationSearch {
		if reply.Len() > 0 {
			reply.WriteString(" ")
		}
		reply.WriteString(noStationNotice)
	}

	if len(result.Pois) > 0 {
		if reply.Len() > 0 {
			reply.WriteString("\n")
		}
		reply.WriteString("附近还有 ")
		reply.WriteString(formatInt(len(result.Pois)))
		reply.WriteString(" 个可选地点：")
		for _, poi := range result.Pois {
			reply.WriteString("\n· ")
			reply.WriteString(poi.Name)
			reply.WriteString("（")
			reply.WriteString(poi.Category)
			reply.WriteString("）")
			reply.WriteString(distanceText(poi.DistanceMeter))
		}
	} else if containsAny(message, "吃", "餐", "饭", "咖啡", "便利", "商场", "购物") {
		if reply.Len() > 0 {
			reply.WriteString("\n")
		}
		reply.WriteString(noPoiNotice)
	}

	if result.Route != nil {
		if reply.Len() > 0 {
			reply.WriteString("\n")
		}
		reply.WriteString("到 ")
		reply.WriteString(result.Route.DestinationName)
		reply.WriteString(" 约 ")
		reply.WriteString(distanceText(result.Route.DistanceMeter))
		reply.WriteString("，预计 ")
		reply.WriteString(durationText(result.Route.DurationSecond))
		reply.WriteString("。")
	}

	if reply.Len() == 0 {
		reply.WriteString(llmUnavailableNotice)
	}
	return reply.String()
}

// systemMessage builds the system turn.
func (s *Service) systemMessage() Message {
	return Message{Role: RoleSystem, Content: systemPrompt()}
}

// userMessage builds the user turn, stating what the client already told us so
// the model does not ask the user to repeat it.
func (s *Service) userMessage(message string, conv Context) Message {
	content := message
	if conv.HasLocation() {
		content += "\n（用户已授权浏览器定位，位置由系统提供，无需再询问位置。）"
	} else {
		content += "\n（用户未提供定位，如需精确结果可建议其授权定位或直接给出地址。）"
	}
	if conv.ChargerType != nil {
		if *conv.ChargerType == 1 {
			content += "\n（用户偏好快充。）"
		} else {
			content += "\n（用户偏好慢充。）"
		}
	}
	return Message{Role: RoleUser, Content: content}
}

// toolSpecs describes every registered tool to the model.
func (s *Service) toolSpecs() []ToolSpec {
	specs := make([]ToolSpec, 0, len(s.tools))
	for _, tool := range s.tools {
		specs = append(specs, ToolSpec{
			Name:           tool.Name(),
			Description:    tool.Description(),
			ParametersJSON: tool.ParametersSchema(),
		})
	}
	return specs
}

// —— deterministic helpers ——

// poiCategoryFor infers the search category from the user's own words.
//
// Without it, "附近哪里能喝咖啡" would fall back to the default restaurant
// search whenever the model is unavailable, which is exactly the case this path
// exists for.
func poiCategoryFor(message string) string {
	switch {
	case containsAny(message, "咖啡"):
		return "咖啡"
	case containsAny(message, "吃", "餐", "饭", "美食"):
		return "餐饮"
	case containsAny(message, "便利", "超市"):
		return "便利店"
	case containsAny(message, "商场", "购物", "买"):
		return "商场"
	default:
		return ""
	}
}

// requiresStationAnchor reports whether a planned call needs a value that only
// a station search can produce.
func requiresStationAnchor(name string, arguments Arguments, conv Context) bool {
	switch name {
	case toolRoute:
		return !hasArgument(arguments, "destinationLatitudeE6")
	case toolStationDetail:
		return !hasArgument(arguments, "stationId")
	case toolPoiSearch:
		return !hasArgument(arguments, "latitudeE6") && !conv.HasLocation()
	default:
		return false
	}
}

// containsAny reports whether the message mentions any of the keywords.
func containsAny(message string, keywords ...string) bool {
	for _, keyword := range keywords {
		if strings.Contains(message, keyword) {
			return true
		}
	}
	return false
}

// trimReply collapses whitespace and bounds the reply.
//
// The bound counts characters, not bytes: a Chinese reply cut at a byte offset
// would end mid-character, and the model's own length instruction is stated in
// characters.
func trimReply(value string, maxLength int) string {
	text := strings.TrimSpace(value)
	if text == "" {
		return ""
	}
	if utf8.RuneCountInString(text) <= maxLength {
		return text
	}
	runes := []rune(text)
	return strings.TrimSpace(string(runes[:maxLength])) + "…"
}

// formatInt and formatInt64 keep the reply assembly free of strconv noise.
func formatInt(value int) string { return strconv.Itoa(value) }

func formatInt64(value int64) string { return strconv.FormatInt(value, 10) }
