package agent

import (
	"context"
	"errors"
	"strings"
	"testing"

	"github.com/heguangV/charging-station-platform/backend/internal/station"
)

// The orchestration rules that make the tools usable by a model: what runs
// first, what runs only once, what is capped, and what a partial failure does
// to the rest of the answer.

// A model cannot know a station id, so a plan that jumps straight to a detail
// would fail for a reason that has nothing to do with the question. The search
// it did not ask for is what makes the detail work.
func TestChatSearchesFirstWhenADetailNeedsAStationID(t *testing.T) {
	stations := &fakeStations{summaries: []station.Summary{sampleStation(7, "中关村站")}}
	llm := &fakeLLM{
		available: true,
		responses: []ChatResponse{
			{ToolCalls: []ToolCall{{Name: toolStationDetail, ArgumentsJSON: `{}`}}},
			{Content: "该站有 10 个充电桩。"},
		},
	}
	service := newTestService(t, Config{Stations: stations, LLM: llm})

	result := service.Chat(context.Background(), "这个站有多少充电桩", Context{Location: stationAt()})

	if len(stations.searches) != 1 {
		t.Fatalf("station searches = %d, want the injected anchor search", len(stations.searches))
	}
	if strings.Join(result.Tools, ",") != "station_search,station_detail" {
		t.Fatalf("tools = %v, want the search to run before the detail", result.Tools)
	}
	if len(stations.detailIDs) != 1 || stations.detailIDs[0] != 7 {
		t.Fatalf("detail ids = %v, want the id the search found", stations.detailIDs)
	}
}

// The anchor also carries the coordinates a route needs, which the model has no
// way to supply.
func TestChatInjectsTheAnchorIntoARoute(t *testing.T) {
	stations := &fakeStations{summaries: []station.Summary{sampleStation(3, "望京站")}}
	routes := &fakeRoutes{route: Route{DestinationName: "望京站", DistanceMeter: 4200, DurationSecond: 600}}
	llm := &fakeLLM{
		available: true,
		responses: []ChatResponse{
			{ToolCalls: []ToolCall{{Name: toolRoute, ArgumentsJSON: `{"mode":"driving"}`}}},
			{Content: "约 4.2 公里。"},
		},
	}
	service := newTestService(t, Config{Stations: stations, Routes: routes, LLM: llm})

	service.Chat(context.Background(), "导航到最近的充电站", Context{Location: stationAt()})

	if len(routes.requests) != 1 {
		t.Fatalf("route requests = %d, want 1", len(routes.requests))
	}
	request := routes.requests[0]
	if request.Destination.LatitudeE6 != 39977680 || request.Destination.LongitudeE6 != 116316417 {
		t.Fatalf("destination = %+v, want the anchor station's coordinates", request.Destination)
	}
	if request.DestinationName != "望京站" {
		t.Fatalf("destination name = %q, want the anchor station's name", request.DestinationName)
	}
}

// Asking for the same tool three times must not spend the request budget three
// times: one call is enough to answer, and the model's repetition is noise.
func TestChatRunsEachToolAtMostOnce(t *testing.T) {
	stations := &fakeStations{}
	llm := &fakeLLM{
		available: true,
		responses: []ChatResponse{
			{ToolCalls: []ToolCall{
				{Name: toolStationSearch, ArgumentsJSON: `{}`},
				{Name: toolStationSearch, ArgumentsJSON: `{"limit":3}`},
				{Name: toolStationSearch, ArgumentsJSON: `{"limit":5}`},
			}},
			{Content: "好。"},
		},
	}
	service := newTestService(t, Config{Stations: stations, LLM: llm})

	result := service.Chat(context.Background(), "附近哪有充电站", Context{Location: stationAt()})

	if len(stations.searches) != 1 {
		t.Fatalf("station searches = %d, want 1", len(stations.searches))
	}
	if len(result.Tools) != 1 {
		t.Fatalf("tools = %v, want one entry", result.Tools)
	}
}

// The tool budget is a hard bound: a model that asks for more work than one
// request may do gets the first calls and no more.
func TestChatStopsAtTheToolBudget(t *testing.T) {
	stations := &fakeStations{summaries: []station.Summary{sampleStation(1, "站")}}
	pois := &fakePois{available: true}
	routes := &fakeRoutes{route: Route{DestinationName: "站", DistanceMeter: 100}}
	llm := &fakeLLM{
		available: true,
		responses: []ChatResponse{
			{ToolCalls: []ToolCall{
				{Name: toolStationSearch, ArgumentsJSON: `{}`},
				{Name: toolStationDetail, ArgumentsJSON: `{}`},
				{Name: toolPoiSearch, ArgumentsJSON: `{}`},
				{Name: toolRoute, ArgumentsJSON: `{}`},
			}},
			{Content: "好。"},
		},
	}
	service := newTestService(t, Config{
		Stations: stations, Pois: pois, Routes: routes, LLM: llm,
		Limits: Limits{MaxToolCalls: 2},
	})

	result := service.Chat(context.Background(), "找个带餐厅的快充站", Context{Location: stationAt()})

	if len(result.Tools) != 2 {
		t.Fatalf("tools = %v, want the budget of 2 to be respected", result.Tools)
	}
}

// The response is bounded: a station that appears in two tools' results must
// not be counted twice, and the caps are what keep a card list readable.
func TestChatDeduplicatesStationsAndRespectsTheCaps(t *testing.T) {
	shared := sampleStation(1, "阿站")
	stations := &fakeStations{summaries: []station.Summary{
		shared, sampleStation(2, "乙站"), sampleStation(3, "丙站"), sampleStation(4, "丁站"),
	}}
	service := newTestService(t, Config{
		Stations: stations,
		Limits:   Limits{MaxStations: 2},
	})

	result := service.Chat(context.Background(), "附近哪有充电站", Context{Location: stationAt()})

	if len(result.Stations) != 2 {
		t.Fatalf("stations = %d, want the cap of 2", len(result.Stations))
	}
	if result.Stations[0].ID != 1 || result.Stations[1].ID != 2 {
		t.Fatalf("stations = %v, want the first two", result.Stations)
	}
}

// The places list has its own cap, because a provider may return far more than
// a card list can show.
func TestChatCapsThePlaces(t *testing.T) {
	found := make([]Poi, 0, 9)
	for index := 1; index <= 9; index++ {
		found = append(found, Poi{ID: string(rune('a' + index)), Name: "地点", DistanceMeter: 100})
	}
	service := newTestService(t, Config{
		Stations: &fakeStations{},
		Pois:     &fakePois{available: true, found: found},
		Limits:   Limits{MaxPois: 3},
	})

	result := service.Chat(context.Background(), "附近有什么咖啡店", Context{Location: stationAt()})

	if len(result.Pois) != 3 {
		t.Fatalf("pois = %d, want the cap of 3", len(result.Pois))
	}
}

// The buttons are derived from what was found, so a client never has to guess
// what to offer next.
func TestChatBuildsActionsFromWhatItFound(t *testing.T) {
	service := newTestService(t, Config{
		Stations: &fakeStations{summaries: []station.Summary{sampleStation(5, "中关村站")}},
		Routes: &fakeRoutes{route: Route{
			DestinationName: "中关村站", DistanceMeter: 2300, DurationSecond: 480,
			BrowserURL: "https://apis.map.qq.com/uri/v1/routeplan?to=中关村站",
		}},
	})

	result := service.Chat(context.Background(), "导航到最近的充电站", Context{Location: stationAt()})

	if len(result.Actions) != 2 {
		t.Fatalf("actions = %+v, want one per station and one navigation", result.Actions)
	}
	if result.Actions[0].Type != ActionOpenStation || result.Actions[0].TargetID != "5" {
		t.Fatalf("first action = %+v", result.Actions[0])
	}
	if result.Actions[1].Type != ActionNavigate || result.Actions[1].URL == "" {
		t.Fatalf("second action = %+v", result.Actions[1])
	}
}

// A straight-line estimate is reported as a degradation. Telling a user a
// driving distance that is not a driving distance is worse than telling them
// the map service is down.
func TestChatMarksAFallbackRouteAsDegraded(t *testing.T) {
	service := newTestService(t, Config{
		Stations: &fakeStations{summaries: []station.Summary{sampleStation(1, "站")}},
		Routes: &fakeRoutes{route: Route{
			DestinationName: "站", DistanceMeter: 2300, DurationSecond: 480,
			Provider: ProviderLocalFallback, Fallback: true,
		}},
	})

	result := service.Chat(context.Background(), "导航到最近的充电站", Context{Location: stationAt()})

	if result.Route == nil || !result.Route.Fallback {
		t.Fatalf("route = %+v, want the fallback flag preserved", result.Route)
	}
	if !result.Degraded {
		t.Fatal("a fallback route must be reported as degraded")
	}
	if !strings.Contains(result.Reply, mapUnavailableNotice) {
		t.Fatalf("reply = %q, want the map notice", result.Reply)
	}
	if strings.Count(result.Reply, mapUnavailableNotice) != 1 {
		t.Fatalf("the map notice must appear once: %q", result.Reply)
	}
}

// A failed place search degrades that part without discarding the station
// results the user actually asked for.
func TestChatKeepsWorkingToolsWhenOneFails(t *testing.T) {
	service := newTestService(t, Config{
		Stations: &fakeStations{summaries: []station.Summary{sampleStation(1, "中关村站")}},
		Pois:     &fakePois{available: true, err: errors.New("the map provider is down")},
	})

	result := service.Chat(context.Background(), "找个附近能吃饭的充电站", Context{Location: stationAt()})

	if len(result.Stations) != 1 {
		t.Fatalf("stations = %d, want the station result to survive the other tool's failure", len(result.Stations))
	}
	if !result.Degraded {
		t.Fatal("a failed tool must be reported as degraded")
	}
	if !strings.Contains(result.Reply, "中关村站") {
		t.Fatalf("reply = %q, want the station it did find", result.Reply)
	}
}

// A search that could not run is not a search that found nothing. Saying "no
// stations nearby" when the database was unreachable would be a confident
// answer to a question nobody asked.
func TestChatDistinguishesAFailedSearchFromAnEmptyOne(t *testing.T) {
	failedCases := map[string]struct {
		stations *fakeStations
		want     string
	}{
		"search failed": {stations: &fakeStations{searchErr: errors.New("database is down")}, want: stationQueryFailedNotice},
		"search empty":  {stations: &fakeStations{}, want: noStationNotice},
	}
	for name, tc := range failedCases {
		t.Run(name, func(t *testing.T) {
			service := newTestService(t, Config{Stations: tc.stations})
			result := service.Chat(context.Background(), "附近哪有充电站", Context{Location: stationAt()})
			if !strings.Contains(result.Reply, tc.want) {
				t.Fatalf("reply = %q, want %q", result.Reply, tc.want)
			}
		})
	}
}

// Without a position and without a result, the only useful thing left to say is
// how to get a better answer.
func TestChatAsksForALocationWhenItHasNoneAndFoundNothing(t *testing.T) {
	service := newTestService(t, Config{Stations: &fakeStations{}})

	result := service.Chat(context.Background(), "附近哪有充电站", Context{})

	if !strings.Contains(result.Reply, noLocationNotice) {
		t.Fatalf("reply = %q, want the location hint", result.Reply)
	}
}

// —— positions and datums ——

// Everything downstream speaks GCJ-02, so a browser position is converted once
// on the way in - and the tool must receive the converted value, not the raw
// one.
func TestChatConvertsABrowserPositionBeforeSearching(t *testing.T) {
	stations := &fakeStations{}
	converter := &fakeConverter{}
	service := newTestService(t, Config{Stations: stations, Converter: converter})

	service.Chat(context.Background(), "附近哪有充电站", Context{Location: stationAt(), WGS84: true})

	if len(converter.inputs) != 1 {
		t.Fatalf("conversions = %d, want 1", len(converter.inputs))
	}
	if len(stations.searches) != 1 {
		t.Fatalf("searches = %d, want 1", len(stations.searches))
	}
	got := stations.searches[0]
	// The fake shifts by +500/+600 E6 units.
	if got.Latitude != 39.97818 || got.Longitude != 116.317017 {
		t.Fatalf("searched from (%v, %v), want the converted position", got.Latitude, got.Longitude)
	}
}

// A position that cannot be converted is still used, and the answer says it is
// approximate. The offset is a few hundred metres - enough to reorder two
// stations that are close together, and far less harmful than dropping the
// position, which would turn "the nearest three stations" into three arbitrary
// ones.
func TestChatKeepsAPositionItCannotConvertAndSaysSo(t *testing.T) {
	stations := &fakeStations{}
	service := newTestService(t, Config{
		Stations:  stations,
		Converter: &fakeConverter{err: errors.New("conversion unavailable")},
	})

	result := service.Chat(context.Background(), "附近哪有充电站", Context{Location: stationAt(), WGS84: true})

	if !result.Degraded {
		t.Fatal("an unconverted position must be reported as degraded")
	}
	if len(stations.searches) != 1 {
		t.Fatalf("searches = %d, want 1", len(stations.searches))
	}
	if !stations.searches[0].HasLocation {
		t.Fatal("the search must still use the position it was given")
	}
	if stations.searches[0].Latitude != 39.97768 {
		t.Fatalf("searched from %v, want the position as given", stations.searches[0].Latitude)
	}
}

// A GCJ-02 position needs no conversion, and must not be shifted.
func TestChatLeavesAPlatformPositionAlone(t *testing.T) {
	stations := &fakeStations{}
	converter := &fakeConverter{}
	service := newTestService(t, Config{Stations: stations, Converter: converter})

	service.Chat(context.Background(), "附近哪有充电站", Context{Location: stationAt()})

	if len(converter.inputs) != 0 {
		t.Fatalf("conversions = %d, want none for a GCJ-02 position", len(converter.inputs))
	}
	if stations.searches[0].Latitude != 39.97768 {
		t.Fatalf("searched from %v, want the position as given", stations.searches[0].Latitude)
	}
}

// Without a map key the platform cannot correct a browser position at all. The
// search still runs from it - that is the difference between a slightly
// approximate answer and no answer - and the degradation is reported.
func TestChatUsesABrowserPositionWhenNoConverterIsConfigured(t *testing.T) {
	stations := &fakeStations{}
	service := newTestService(t, Config{Stations: stations})

	result := service.Chat(context.Background(), "附近哪有充电站", Context{Location: stationAt(), WGS84: true})

	if !result.Degraded {
		t.Fatal("an unconverted position must be reported as degraded")
	}
	if !stations.searches[0].HasLocation {
		t.Fatal("the search must still use the position it was given")
	}
}

// —— the user's preference ——

// A stated preference has to reach the search, and an explicit tool argument
// must win over it.
func TestChatAppliesTheUsersChargerPreference(t *testing.T) {
	fast := 1
	stations := &fakeStations{}
	service := newTestService(t, Config{Stations: stations})

	service.Chat(context.Background(), "附近哪有充电站", Context{Location: stationAt(), ChargerType: &fast})

	if stations.searches[0].ConnectorType != "DC" {
		t.Fatalf("connector = %q, want DC for the fast preference", stations.searches[0].ConnectorType)
	}
}

// —— the reply itself ——

// The reply is shown as-is, so it is trimmed to a length a card can hold and
// never cut in the middle of a character.
func TestChatTrimsALongModelReplyOnACharacterBoundary(t *testing.T) {
	long := strings.Repeat("充", 900)
	llm := &fakeLLM{
		available: true,
		responses: []ChatResponse{
			{ToolCalls: []ToolCall{{Name: toolStationSearch, ArgumentsJSON: `{}`}}},
			{Content: long},
		},
	}
	service := newTestService(t, Config{
		Stations: &fakeStations{summaries: []station.Summary{sampleStation(1, "站")}},
		LLM:      llm,
		Limits:   Limits{MaxReplyLength: 20},
	})

	result := service.Chat(context.Background(), "附近哪有充电站", Context{Location: stationAt()})

	if result.Reply != strings.Repeat("充", 20)+"…" {
		t.Fatalf("reply = %q, want twenty characters and an ellipsis", result.Reply)
	}
}

// A model that returns only whitespace has not answered, and the deterministic
// wording must stand in.
func TestChatFallsBackWhenTheModelReturnsNothing(t *testing.T) {
	llm := &fakeLLM{
		available: true,
		responses: []ChatResponse{
			{ToolCalls: []ToolCall{{Name: toolStationSearch, ArgumentsJSON: `{}`}}},
			{Content: "   \n  "},
		},
	}
	service := newTestService(t, Config{
		Stations: &fakeStations{summaries: []station.Summary{sampleStation(1, "中关村站")}},
		LLM:      llm,
	})

	result := service.Chat(context.Background(), "附近哪有充电站", Context{Location: stationAt()})

	if result.LLMUsed {
		t.Fatal("an empty completion is not a reply")
	}
	if !result.Degraded {
		t.Fatal("an empty completion is a degradation")
	}
	if !strings.Contains(result.Reply, "中关村站") {
		t.Fatalf("reply = %q, want the deterministic wording", result.Reply)
	}
}

// The tool list is part of the response contract, so its order is stable and
// reflects what actually ran.
func TestToolNamesAreStable(t *testing.T) {
	service := newTestService(t, Config{Stations: &fakeStations{}})
	got := strings.Join(service.ToolNames(), ",")
	if got != "station_search,station_detail,poi_search,route" {
		t.Fatalf("ToolNames() = %q", got)
	}
}

// A service without a station directory cannot ground a single answer, so it
// must not be constructible.
func TestNewServiceRequiresAStationDirectory(t *testing.T) {
	if _, err := NewService(Config{}); err == nil {
		t.Fatal("expected a service without stations to be refused")
	}
}

// The configured search radius has to be the one the provider receives: a
// setting that is plumbed through but never read is worse than no setting,
// because an operator would believe it took effect.
func TestChatUsesTheConfiguredPoiRadius(t *testing.T) {
	pois := &fakePois{available: true}
	service := newTestService(t, Config{
		Stations: &fakeStations{summaries: []station.Summary{sampleStation(1, "站")}},
		Pois:     pois,
		Limits:   Limits{DefaultPoiRadiusMeter: 3500},
	})

	service.Chat(context.Background(), "附近有什么咖啡店", Context{Location: stationAt()})

	if len(pois.queries) != 1 {
		t.Fatalf("poi searches = %d, want 1", len(pois.queries))
	}
	if pois.queries[0].RadiusMeter != 3500 {
		t.Fatalf("radius = %d, want the configured 3500", pois.queries[0].RadiusMeter)
	}
}

// An explicit radius from the model still wins over the configured default.
func TestChatPrefersTheRadiusTheModelAskedFor(t *testing.T) {
	pois := &fakePois{available: true}
	llm := &fakeLLM{
		available: true,
		responses: []ChatResponse{
			{ToolCalls: []ToolCall{{Name: toolPoiSearch, ArgumentsJSON: `{"radiusMeter":800}`}}},
			{Content: "好。"},
		},
	}
	service := newTestService(t, Config{
		Stations: &fakeStations{summaries: []station.Summary{sampleStation(1, "站")}},
		Pois:     pois,
		LLM:      llm,
		Limits:   Limits{DefaultPoiRadiusMeter: 3500},
	})

	service.Chat(context.Background(), "附近有什么咖啡店", Context{Location: stationAt()})

	if len(pois.queries) != 1 {
		t.Fatalf("poi searches = %d, want 1", len(pois.queries))
	}
	if pois.queries[0].RadiusMeter != 800 {
		t.Fatalf("radius = %d, want the model's 800", pois.queries[0].RadiusMeter)
	}
}
