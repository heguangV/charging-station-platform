package agent

import (
	"context"
	"errors"
	"strings"
	"testing"

	"github.com/heguangV/charging-station-platform/backend/internal/station"
)

// The fakes below replace the three things a unit test must never reach:
// PostgreSQL, the map provider and the model. Each one records what it was
// asked, so the orchestration can be asserted rather than inferred.

type fakeStations struct {
	summaries []station.Summary
	detail    station.SummaryDetail
	searchErr error
	detailErr error

	searches  []station.SummaryFilter
	detailIDs []int64
	detailCT  []string
}

func (f *fakeStations) SearchSummaries(_ context.Context, filter station.SummaryFilter) ([]station.Summary, error) {
	f.searches = append(f.searches, filter)
	if f.searchErr != nil {
		return nil, f.searchErr
	}
	return f.summaries, nil
}

func (f *fakeStations) GetStationSummary(_ context.Context, stationID int64, chargerType string, _ int) (station.SummaryDetail, error) {
	f.detailIDs = append(f.detailIDs, stationID)
	f.detailCT = append(f.detailCT, chargerType)
	if f.detailErr != nil {
		return station.SummaryDetail{}, f.detailErr
	}
	return f.detail, nil
}

type fakePois struct {
	available bool
	found     []Poi
	err       error
	queries   []PoiQuery
}

func (f *fakePois) Available() bool { return f.available }

func (f *fakePois) SearchPois(_ context.Context, query PoiQuery) ([]Poi, error) {
	f.queries = append(f.queries, query)
	if f.err != nil {
		return nil, f.err
	}
	return f.found, nil
}

type fakeRoutes struct {
	route    Route
	err      error
	requests []RouteRequest
}

func (f *fakeRoutes) PlanRoute(_ context.Context, request RouteRequest) (Route, error) {
	f.requests = append(f.requests, request)
	if f.err != nil {
		return Route{}, f.err
	}
	return f.route, nil
}

type fakeLLM struct {
	available bool
	responses []ChatResponse
	errs      []error
	requests  []ChatRequest
}

func (f *fakeLLM) Available() bool { return f.available }

func (f *fakeLLM) Chat(_ context.Context, request ChatRequest) (ChatResponse, error) {
	index := len(f.requests)
	f.requests = append(f.requests, request)
	if index < len(f.errs) && f.errs[index] != nil {
		return ChatResponse{}, f.errs[index]
	}
	if index < len(f.responses) {
		return f.responses[index], nil
	}
	return ChatResponse{}, errors.New("no scripted response")
}

type fakeConverter struct {
	err    error
	inputs []Location
}

func (f *fakeConverter) ToGCJ02(position Location) (Location, error) {
	f.inputs = append(f.inputs, position)
	if f.err != nil {
		return Location{}, f.err
	}
	// A real conversion moves the point by a few hundred metres; the test only
	// needs to see that it happened and which position was handed over.
	return Location{LatitudeE6: position.LatitudeE6 + 500, LongitudeE6: position.LongitudeE6 + 600}, nil
}

func (f *fakeConverter) ToWGS84(position Location) (Location, error) { return position, nil }

// sampleStation is one recommendation as the station domain would return it.
func sampleStation(id int64, name string) station.Summary {
	return station.Summary{
		ID:                         id,
		Code:                       "ST" + name,
		Name:                       name,
		Address:                    "北京市海淀区",
		Status:                     "OPEN",
		LatitudeE6:                 39977680,
		LongitudeE6:                116316417,
		ChargerCount:               10,
		IdleChargerCount:           3,
		OperationalChargerCount:    9,
		FastChargerCount:           6,
		SlowChargerCount:           4,
		ChargerTypes:               []string{"AC", "DC"},
		TotalPriceCentPerKwh:       135,
		ElectricityPriceCentPerKwh: 85,
		ServicePriceCentPerKwh:     50,
		DistanceMeter:              2300,
		HasDistance:                true,
	}
}

func newTestService(t *testing.T, cfg Config) *Service {
	t.Helper()
	service, err := NewService(cfg)
	if err != nil {
		t.Fatalf("NewService() error = %v", err)
	}
	return service
}

func stationAt() *Location {
	return &Location{LatitudeE6: 39977680, LongitudeE6: 116316417}
}

// —— the deterministic path ——

// A deployment with no model at all still has to answer, and it has to say that
// it did so without one.
func TestChatWithoutAModelAnswersFromToolDataAndSaysSo(t *testing.T) {
	stations := &fakeStations{summaries: []station.Summary{sampleStation(1, "中关村站")}}
	service := newTestService(t, Config{Stations: stations})

	result := service.Chat(context.Background(), "附近哪有充电站", Context{Location: stationAt()})

	if result.Degraded != true {
		t.Fatal("a reply written without a model must be reported as degraded")
	}
	if result.LLMUsed {
		t.Fatal("no model was configured, so llmUsed must be false")
	}
	if len(result.Stations) != 1 {
		t.Fatalf("stations = %d, want the one the search returned", len(result.Stations))
	}
	if len(result.Tools) != 1 || result.Tools[0] != toolStationSearch {
		t.Fatalf("tools = %v, want one station_search", result.Tools)
	}
	if !strings.Contains(result.Reply, "中关村站") {
		t.Fatalf("reply must name the station it found: %q", result.Reply)
	}
	if result.Reply == "" {
		t.Fatal("the reply must never be empty")
	}
}

// The reply is the only thing a client is guaranteed to see, so the degraded
// notice has to be in it rather than only in the flag.
func TestChatWithoutAModelOpensWithTheUnavailableNotice(t *testing.T) {
	service := newTestService(t, Config{Stations: &fakeStations{}})
	result := service.Chat(context.Background(), "你好", Context{Location: stationAt()})
	if !strings.Contains(result.Reply, llmUnavailableNotice) {
		t.Fatalf("reply = %q, want the unavailable notice", result.Reply)
	}
}

// A search that matched nothing is a successful search, not a failure: the
// caller gets an empty list and the wording that explains it.
func TestChatReportsAnEmptySearchAsAnEmptyResultNotAFailure(t *testing.T) {
	service := newTestService(t, Config{Stations: &fakeStations{}})
	result := service.Chat(context.Background(), "附近哪有充电站", Context{Location: stationAt()})

	if len(result.Stations) != 0 {
		t.Fatalf("stations = %d, want none", len(result.Stations))
	}
	if !strings.Contains(result.Reply, noStationNotice) {
		t.Fatalf("reply = %q, want the empty-search notice", result.Reply)
	}
	for _, name := range result.Tools {
		if name != toolStationSearch {
			t.Fatalf("unexpected tool %q", name)
		}
	}
}

// An empty result must never be dressed up as an answer: the collections stay
// empty and the caller can tell the difference from a populated one.
func TestChatNeverInventsDataWhenEverythingIsUnavailable(t *testing.T) {
	service := newTestService(t, Config{
		Stations: &fakeStations{searchErr: errors.New("database is down")},
		Pois:     &fakePois{available: false},
		Routes:   &fakeRoutes{},
	})
	result := service.Chat(context.Background(), "附近哪有充电站", Context{Location: stationAt()})

	if len(result.Stations) != 0 || len(result.Pois) != 0 || result.Route != nil {
		t.Fatalf("an unavailable search must not produce data: %+v", result)
	}
	if !result.Degraded {
		t.Fatal("a failed search must be reported as degraded")
	}
	if result.Reply == "" {
		t.Fatal("the reply must never be empty, even when everything failed")
	}
}

// —— deterministic planning ——

// The fallback planner is what answers when there is no model, so its intent
// decisions are part of the contract.
func TestPlanToolsReadsTheQuestionWithoutAModel(t *testing.T) {
	service := newTestService(t, Config{Stations: &fakeStations{}})

	cases := map[string][]string{
		"附近哪有充电站":     {toolStationSearch},
		"导航到最近的充电站":   {toolStationSearch, toolRoute},
		"充电站附近有什么咖啡店": {toolStationSearch, toolPoiSearch},
		"这个站有多少充电桩":   {toolStationSearch, toolStationDetail},
	}
	for message, want := range cases {
		got := service.PlanTools(message)
		if strings.Join(got, ",") != strings.Join(want, ",") {
			t.Fatalf("PlanTools(%q) = %v, want %v", message, got, want)
		}
	}
}

// The category has to come from the user's own words: without it, a question
// about coffee would fall back to a restaurant search exactly when the model is
// unavailable, which is the case this path exists for.
func TestChatInfersThePoiCategoryWithoutAModel(t *testing.T) {
	pois := &fakePois{available: true}
	service := newTestService(t, Config{Stations: &fakeStations{summaries: []station.Summary{sampleStation(1, "站")}}, Pois: pois})

	service.Chat(context.Background(), "充电站附近哪里能喝咖啡", Context{Location: stationAt()})

	if len(pois.queries) != 1 {
		t.Fatalf("poi searches = %d, want 1", len(pois.queries))
	}
	if pois.queries[0].Category != "咖啡" {
		t.Fatalf("category = %q, want 咖啡", pois.queries[0].Category)
	}
}

// —— planning with a model ——

// A model that only talks would leave the client with a sentence and no data,
// so a plan with no tools still runs the deterministic one.
func TestChatRunsTheDeterministicPlanWhenTheModelAsksForNoTool(t *testing.T) {
	stations := &fakeStations{summaries: []station.Summary{sampleStation(1, "站")}}
	llm := &fakeLLM{
		available: true,
		responses: []ChatResponse{
			{Content: "我可以帮你找充电站。"},
			{Content: "附近有一家不错的充电站。"},
		},
	}
	service := newTestService(t, Config{Stations: stations, LLM: llm})

	result := service.Chat(context.Background(), "附近哪有充电站", Context{Location: stationAt()})

	if len(stations.searches) != 1 {
		t.Fatalf("station searches = %d, want the deterministic plan to have run one", len(stations.searches))
	}
	if !result.LLMUsed {
		t.Fatal("the model wrote the reply, so llmUsed must be true")
	}
	if result.Reply != "附近有一家不错的充电站。" {
		t.Fatalf("reply = %q, want the model's wording", result.Reply)
	}
}

// A model that invents a tool name must not be able to reach anything: the name
// is dropped and only registered tools ever run.
func TestChatIgnoresToolsTheModelInvented(t *testing.T) {
	stations := &fakeStations{}
	llm := &fakeLLM{
		available: true,
		responses: []ChatResponse{
			{ToolCalls: []ToolCall{{Name: "delete_all_orders", ArgumentsJSON: `{}`}}},
			{Content: "已完成。"},
		},
	}
	service := newTestService(t, Config{Stations: stations, LLM: llm})

	result := service.Chat(context.Background(), "附近哪有充电站", Context{Location: stationAt()})

	for _, name := range result.Tools {
		if name == "delete_all_orders" {
			t.Fatal("an unregistered tool must never run")
		}
	}
	if len(stations.searches) != 1 {
		t.Fatalf("the deterministic plan should have supplied station_search, searches = %d", len(stations.searches))
	}
}

// The model is shown the tool list with its schemas; a model without them
// cannot choose anything.
func TestChatOffersEveryToolToTheModel(t *testing.T) {
	llm := &fakeLLM{available: true, responses: []ChatResponse{{}, {}}}
	service := newTestService(t, Config{
		Stations: &fakeStations{},
		Pois:     &fakePois{available: true},
		Routes:   &fakeRoutes{},
		LLM:      llm,
	})

	service.Chat(context.Background(), "附近哪有充电站", Context{})

	if len(llm.requests) == 0 {
		t.Fatal("the model was never asked")
	}
	names := make([]string, 0, len(llm.requests[0].Tools))
	for _, spec := range llm.requests[0].Tools {
		names = append(names, spec.Name)
		if spec.ParametersJSON == "" {
			t.Fatalf("tool %s was offered without a schema", spec.Name)
		}
	}
	if strings.Join(names, ",") != "station_search,station_detail,poi_search,route" {
		t.Fatalf("offered tools = %v", names)
	}
}

// A model that plans but cannot write is still a degraded answer, even though
// the tool data is real.
func TestChatDegradesWhenTheModelFailsToWrite(t *testing.T) {
	llm := &fakeLLM{
		available: true,
		responses: []ChatResponse{{ToolCalls: []ToolCall{{Name: toolStationSearch, ArgumentsJSON: `{}`}}}},
		errs:      []error{nil, errors.New("the model timed out")},
	}
	service := newTestService(t, Config{Stations: &fakeStations{summaries: []station.Summary{sampleStation(1, "站")}}, LLM: llm})

	result := service.Chat(context.Background(), "附近哪有充电站", Context{Location: stationAt()})

	if result.LLMUsed {
		t.Fatal("the model did not write the reply")
	}
	if !result.Degraded {
		t.Fatal("a model that could not write must be reported as degraded")
	}
	if result.Reply == "" {
		t.Fatal("the deterministic wording must stand in")
	}
}

// A model that cannot even plan degrades the same way.
func TestChatDegradesWhenTheModelFailsToPlan(t *testing.T) {
	llm := &fakeLLM{available: true, errs: []error{errors.New("connection refused")}}
	service := newTestService(t, Config{Stations: &fakeStations{}, LLM: llm})

	result := service.Chat(context.Background(), "附近哪有充电站", Context{Location: stationAt()})

	if !result.Degraded || result.LLMUsed {
		t.Fatalf("degraded = %v, llmUsed = %v; both must reflect the failure", result.Degraded, result.LLMUsed)
	}
}
