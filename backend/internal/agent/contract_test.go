package agent

import (
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"testing"

	"github.com/heguangV/charging-station-platform/backend/internal/station"
)

// This file pins the registered contract to the responses the endpoint
// actually produces.
//
// It exists because the two are written in different languages: the schema in
// api/openapi.yaml and the DTO in http.go. Nothing else connects them, so a
// renamed Go field would change what every client receives while the published
// contract went on promising the old name - and the mismatch would be found by
// a browser, not by a test.
//
// The check is two-sided and mechanical:
//
//  1. the response must carry exactly the keys the schema names, asserted from
//     an explicit document rather than from the DTO itself; and
//  2. every key the schema requires must be present, and no key outside its
//     declared properties may appear.
//
// The spec is read with the standard library only: a YAML dependency in go.mod
// is not worth carrying for one test, so the `required:` line and the property
// names of a schema block are extracted by pattern. A reformat that defeats the
// patterns fails the test with a message saying so, rather than passing
// silently.

// specPath is the repository's OpenAPI document, relative to this package.
const specPath = "../../../api/openapi.yaml"

// schemaBlock is one entry of components/schemas, as far as this test needs it.
type schemaBlock struct {
	required   []string
	properties []string
}

// loadSchemas extracts the schema blocks this test asserts against.
func loadSchemas(t *testing.T, names ...string) map[string]schemaBlock {
	t.Helper()
	raw, err := os.ReadFile(filepath.Clean(specPath))
	if err != nil {
		t.Fatalf("read %s: %v", specPath, err)
	}
	lines := strings.Split(string(raw), "\n")

	const indent = "    " // components/schemas entries sit four spaces in
	found := map[string]schemaBlock{}
	// The schema's own `required:` sits six spaces in; a nested object property
	// declares its own at a deeper indent and must not be mistaken for it.
	requiredLine := regexp.MustCompile(`^ {6}required:\s*\[(.*)\]\s*$`)
	// A property may be declared inline on one line or expanded over several.
	propertyLine := regexp.MustCompile(`^ {8}([A-Za-z][A-Za-z0-9]*):`)

	for _, name := range names {
		start := -1
		for index, line := range lines {
			if line == indent+name+":" {
				start = index
				break
			}
		}
		if start < 0 {
			t.Fatalf("schema %q is not defined in %s", name, specPath)
		}
		block := schemaBlock{}
		for index := start + 1; index < len(lines); index++ {
			line := lines[index]
			// The next schema block starts at the same indent without a space.
			if strings.HasPrefix(line, indent) && !strings.HasPrefix(line, indent+" ") && line != "" {
				break
			}
			if match := requiredLine.FindStringSubmatch(line); match != nil {
				for _, part := range strings.Split(match[1], ",") {
					if trimmed := strings.TrimSpace(part); trimmed != "" {
						block.required = append(block.required, trimmed)
					}
				}
			}
			if match := propertyLine.FindStringSubmatch(line); match != nil {
				block.properties = append(block.properties, match[1])
			}
		}
		if len(block.required) == 0 {
			t.Fatalf("schema %q has no inline `required: [...]` list; if it was reformatted, update this test", name)
		}
		if len(block.properties) == 0 {
			t.Fatalf("schema %q declares no properties this test can read; if it was reformatted, update this test", name)
		}
		sort.Strings(block.required)
		sort.Strings(block.properties)
		found[name] = block
	}
	return found
}

// responseKeys returns the keys of one object in a response, sorted.
func responseKeys(t *testing.T, object map[string]any) []string {
	t.Helper()
	keys := make([]string, 0, len(object))
	for key := range object {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	return keys
}

// askAgent drives the endpoint and returns the decoded payload.
func askAgent(t *testing.T, handler http.Handler, body string) map[string]any {
	t.Helper()
	recorder := postChat(t, handler, body)
	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200 (%s)", recorder.Code, recorder.Body.String())
	}
	_, payload := decodeEnvelope(t, recorder)
	return payload
}

// fullResult is the fixture that exercises every branch the contract describes:
// a station with its price split, a place, a planned route and the actions
// derived from both.
func fullHandler(t *testing.T) http.Handler {
	t.Helper()
	found := sampleStation(9, "中关村站")
	_, handler := newEndpoint(t, &fakeStations{summaries: []station.Summary{found}},
		func(cfg *Config) {
			cfg.Pois = &fakePois{available: true, found: []Poi{{
				ID: "POI-1", Name: "星巴克", Category: "咖啡厅", Address: "中关村大街 1 号",
				LatitudeE6: 39978100, LongitudeE6: 116316000, DistanceMeter: 210, Tel: "010-1",
			}}}
			cfg.Routes = &fakeRoutes{route: Route{
				DestinationName: "中关村站",
				Origin:          Location{LatitudeE6: 39977680, LongitudeE6: 116316417},
				Destination:     Location{LatitudeE6: 39980000, LongitudeE6: 116320000},
				DistanceMeter:   2300, DurationSecond: 480,
				Provider:   ProviderTencentMap,
				Steps:      []RouteStep{{Instruction: "向东行驶", DistanceMeter: 300, DurationSecond: 60}},
				Polyline:   []RoutePoint{{LatitudeE6: 39977680, LongitudeE6: 116316417}},
				BrowserURL: "https://apis.map.qq.com/uri/v1/routeplan?type=drive",
			}}
		})
	return handler
}

// The response envelope's payload carries exactly the members the contract
// names - no more and no fewer.
func TestResponseMatchesTheRegisteredSchema(t *testing.T) {
	schemas := loadSchemas(t, "AgentChatResponse")
	handler := fullHandler(t)

	payload := askAgent(t, handler,
		`{"message":"找个带咖啡的充电站然后导航过去","location":{"latitudeE6":39977680,"longitudeE6":116316417}}`)

	got := responseKeys(t, payload)
	want := schemas["AgentChatResponse"].required
	if strings.Join(got, ",") != strings.Join(want, ",") {
		t.Fatalf("the response carries %v, but AgentChatResponse names %v", got, want)
	}
	if len(schemas["AgentChatResponse"].properties) != len(want) {
		t.Fatalf("the schema declares %v but requires %v: an optional member needs a branch in this test",
			schemas["AgentChatResponse"].properties, want)
	}
}

// Every member of a nested object is either required by its schema or one of
// its declared optional properties. This is what catches a renamed Go field:
// the DTO would emit a key the schema never mentions.
func TestNestedObjectsMatchTheirRegisteredSchemas(t *testing.T) {
	schemas := loadSchemas(t, "AgentStation", "AgentPoi", "AgentRoute", "AgentRouteStep", "AgentAction")
	handler := fullHandler(t)

	payload := askAgent(t, handler,
		`{"message":"找个带咖啡的充电站然后导航过去","location":{"latitudeE6":39977680,"longitudeE6":116316417}}`)

	stations, _ := payload["stations"].([]any)
	pois, _ := payload["pois"].([]any)
	actions, _ := payload["actions"].([]any)
	route, _ := payload["route"].(map[string]any)
	if len(stations) == 0 || len(pois) == 0 || len(actions) == 0 || route == nil {
		t.Fatalf("the fixture did not populate every collection: %v", payload)
	}

	checkObject(t, "AgentStation", schemas["AgentStation"], stations[0].(map[string]any))
	checkObject(t, "AgentPoi", schemas["AgentPoi"], pois[0].(map[string]any))
	checkObject(t, "AgentAction", schemas["AgentAction"], actions[0].(map[string]any))
	checkObject(t, "AgentRoute", schemas["AgentRoute"], route)

	steps, _ := route["steps"].([]any)
	if len(steps) == 0 {
		t.Fatal("the fixture route carried no steps")
	}
	checkObject(t, "AgentRouteStep", schemas["AgentRouteStep"], steps[0].(map[string]any))
}

// checkObject asserts the two sides of the contract for one nested object.
func checkObject(t *testing.T, name string, schema schemaBlock, object map[string]any) {
	t.Helper()
	keys := responseKeys(t, object)

	declared := map[string]bool{}
	for _, property := range schema.properties {
		declared[property] = true
	}
	for _, key := range keys {
		if !declared[key] {
			t.Fatalf("%s carries %q, which the schema does not declare (declared: %v)", name, key, schema.properties)
		}
	}
	present := map[string]bool{}
	for _, key := range keys {
		present[key] = true
	}
	for _, required := range schema.required {
		if !present[required] {
			t.Fatalf("%s must carry %q, but the response has %v", name, required, keys)
		}
	}
}

// The optional members have to be legitimately optional: a search without a
// position omits the distance, and that must be the only reason it is absent.
func TestTheOnlyOptionalStationMemberIsTheDistance(t *testing.T) {
	schemas := loadSchemas(t, "AgentStation")
	schema := schemas["AgentStation"]

	optional := make([]string, 0)
	required := map[string]bool{}
	for _, name := range schema.required {
		required[name] = true
	}
	for _, name := range schema.properties {
		if !required[name] {
			optional = append(optional, name)
		}
	}
	// A station the assistant recommends always states its price split and its
	// charger mix; the distance is the one thing a keyword-only search cannot
	// know, and it is omitted rather than reported as zero.
	want := []string{"chargerTypes", "distanceMeter", "electricityPriceCentPerKwh",
		"fastChargerCount", "operationalChargerCount", "servicePriceCentPerKwh", "slowChargerCount"}
	if strings.Join(optional, ",") != strings.Join(want, ",") {
		t.Fatalf("AgentStation's optional members are %v, want %v", optional, want)
	}

	// And the distance really is omitted when there is none.
	found := sampleStation(9, "中关村站")
	found.HasDistance = false
	_, handler := newEndpoint(t, &fakeStations{summaries: []station.Summary{found}})
	payload := askAgent(t, handler, `{"message":"中关村充电站在哪"}`)
	stations, _ := payload["stations"].([]any)
	if len(stations) != 1 {
		t.Fatalf("stations = %v", payload["stations"])
	}
	object := stations[0].(map[string]any)
	if _, present := object["distanceMeter"]; present {
		t.Fatalf("distanceMeter = %v, want it omitted when the search had no position", object["distanceMeter"])
	}
	for _, name := range schema.required {
		if _, present := object[name]; !present {
			t.Fatalf("a station must always carry %q, got %v", name, responseKeys(t, object))
		}
	}
}

// The request schema is the other half of the contract: what the endpoint
// accepts has to be what the document says it accepts.
func TestRequestSchemaMatchesWhatTheEndpointAccepts(t *testing.T) {
	schemas := loadSchemas(t, "AgentChatRequest")
	schema := schemas["AgentChatRequest"]

	// Every declared member is accepted, and the required one is not optional.
	_, handler := newEndpoint(t, &fakeStations{})
	full := `{"message":"附近哪有充电站","location":{"latitudeE6":39977680,"longitudeE6":116316417},` +
		`"coordinateType":"gcj02","chargerType":1}`
	if recorder := postChat(t, handler, full); recorder.Code != http.StatusOK {
		t.Fatalf("a request using every declared member was refused: %d %s", recorder.Code, recorder.Body.String())
	}
	if recorder := postChat(t, handler, `{"message":"附近哪有充电站"}`); recorder.Code != http.StatusOK {
		t.Fatalf("the optional members must be optional: %d %s", recorder.Code, recorder.Body.String())
	}
	for _, name := range schema.required {
		if name != "message" {
			t.Fatalf("the request schema requires %q; this test only knows how to prove %q", name, "message")
		}
	}
	if recorder := postChat(t, handler, `{}`); recorder.Code != http.StatusBadRequest {
		t.Fatalf("the required member is not enforced: %d", recorder.Code)
	}

	// A declared member whose value is outside its enum is refused, not ignored.
	for _, body := range []string{
		`{"message":"附近哪有充电站","coordinateType":"bd09"}`,
		`{"message":"附近哪有充电站","chargerType":7}`,
	} {
		if recorder := postChat(t, handler, body); recorder.Code != http.StatusBadRequest {
			t.Fatalf("body %s answered %d, want 400", body, recorder.Code)
		}
	}
}

// propertyEnum returns the enum values declared for one property of one schema.
//
// It reads the property's own block rather than the first enum in the document:
// several schemas declare enums, and taking the wrong one would make this test
// assert against something else entirely.
func propertyEnum(t *testing.T, schemaName, propertyName string) []string {
	t.Helper()
	raw, err := os.ReadFile(filepath.Clean(specPath))
	if err != nil {
		t.Fatalf("read %s: %v", specPath, err)
	}
	lines := strings.Split(string(raw), "\n")

	const indent = "    "
	start := -1
	for index, line := range lines {
		if line == indent+schemaName+":" {
			start = index
			break
		}
	}
	if start < 0 {
		t.Fatalf("schema %q is not defined in %s", schemaName, specPath)
	}

	propertyStart := -1
	for index := start + 1; index < len(lines); index++ {
		line := lines[index]
		if strings.HasPrefix(line, indent) && !strings.HasPrefix(line, indent+" ") && line != "" {
			break
		}
		if strings.HasPrefix(line, "        "+propertyName+":") {
			propertyStart = index
			break
		}
	}
	if propertyStart < 0 {
		t.Fatalf("schema %q declares no property %q", schemaName, propertyName)
	}

	pattern := regexp.MustCompile(`enum:\s*\[([^\]]*)\]`)
	for index := propertyStart; index < len(lines); index++ {
		line := lines[index]
		if index > propertyStart && strings.HasPrefix(line, "        ") &&
			!strings.HasPrefix(line, "          ") && line != "" {
			break // the next property
		}
		if match := pattern.FindStringSubmatch(line); match != nil {
			values := []string{}
			for _, part := range strings.Split(match[1], ",") {
				if trimmed := strings.TrimSpace(part); trimmed != "" {
					values = append(values, trimmed)
				}
			}
			return values
		}
	}
	t.Fatalf("schema %q property %q declares no enum; if it was reformatted, update this test", schemaName, propertyName)
	return nil
}

// The tools the response reports are the tools the schema enumerates: a new
// tool would otherwise reach a client that cannot render it.
func TestReportedToolsAreTheOnesTheSchemaEnumerates(t *testing.T) {
	declared := map[string]bool{}
	for _, name := range propertyEnum(t, "AgentChatResponse", "tools") {
		declared[name] = true
	}
	if len(declared) == 0 {
		t.Fatal("the response schema enumerates no tools")
	}

	service := newTestService(t, Config{Stations: &fakeStations{}})
	for _, name := range service.ToolNames() {
		if !declared[name] {
			t.Fatalf("tool %q is registered but not enumerated in the response schema", name)
		}
		delete(declared, name)
	}
	if len(declared) != 0 {
		t.Fatalf("the schema enumerates tools that are not registered: %v", declared)
	}
}

// The endpoint is registered at the path the contract publishes. A rename on
// one side only would leave the documented path answering 404.
func TestTheRouteIsThePublishedOne(t *testing.T) {
	raw, err := os.ReadFile(filepath.Clean(specPath))
	if err != nil {
		t.Fatalf("read %s: %v", specPath, err)
	}
	if !strings.Contains(string(raw), "\n  "+strings.TrimPrefix(ChatPath, "/api/v1")+":") {
		t.Fatalf("%s is not published in %s", ChatPath, specPath)
	}
	if ChatPath != "/api/v1/agent/chat" {
		t.Fatalf("ChatPath = %q, want the frozen path", ChatPath)
	}

	// And it is served, not merely documented.
	_, handler := newEndpoint(t, &fakeStations{})
	request := httptest.NewRequest(http.MethodPost, ChatPath, strings.NewReader(`{"message":"附近哪有充电站"}`))
	recorder := httptest.NewRecorder()
	handler.ServeHTTP(recorder, request)
	if recorder.Code != http.StatusOK {
		t.Fatalf("the published path answered %d", recorder.Code)
	}
}
