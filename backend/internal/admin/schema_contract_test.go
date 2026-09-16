package admin

import (
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"testing"

	"github.com/heguangV/charging-station-platform/backend/internal/auth"
)

// This file checks the registered schemas against the responses the handlers
// actually produce.
//
// It exists because of a real defect: StationRecord had no JSON tags, so
// POST /admin/stations answered with Go field names (ID, Code, Status) while the
// operation was registered as returning a schema whose properties are camelCase,
// and nothing noticed until a status-change test compared the two.
//
// The check is deliberately two-sided and mechanical:
//
//  1. the response must carry exactly the keys the contract names (no missing
//     property, no undeclared extra), asserted from an explicit list; and
//  2. the schema in api/openapi.yaml must require exactly that list, so the test
//     and the spec cannot drift apart in either direction.
//
// The spec is read with the standard library only: pulling a YAML dependency into
// go.mod for one test is not worth it, so the `required:` line of the schema
// block is extracted by pattern. If that line is reformatted the test fails with
// a message saying so rather than passing silently.

// specPath is the repository's OpenAPI document, relative to this package.
const specPath = "../../../api/openapi.yaml"

// schemaRequiredList returns the inline `required: [...]` list of one schema
// block in components/schemas.
func schemaRequiredList(t *testing.T, schemaName string) []string {
	t.Helper()
	raw, err := os.ReadFile(filepath.Clean(specPath))
	if err != nil {
		t.Fatalf("read %s: %v", specPath, err)
	}
	lines := strings.Split(string(raw), "\n")

	const indent = "    " // components/schemas entries sit four spaces in
	blockStart := -1
	for i, line := range lines {
		if line == indent+schemaName+":" {
			blockStart = i
			break
		}
	}
	if blockStart < 0 {
		t.Fatalf("schema %q is not defined in %s", schemaName, specPath)
	}

	pattern := regexp.MustCompile(`^\s+required:\s*\[(.*)\]\s*$`)
	for i := blockStart + 1; i < len(lines); i++ {
		line := lines[i]
		if strings.HasPrefix(line, indent) && !strings.HasPrefix(line, indent+" ") && line != "" {
			break // next schema block
		}
		if match := pattern.FindStringSubmatch(line); match != nil {
			parts := strings.Split(match[1], ",")
			out := make([]string, 0, len(parts))
			for _, part := range parts {
				if trimmed := strings.TrimSpace(part); trimmed != "" {
					out = append(out, trimmed)
				}
			}
			sort.Strings(out)
			return out
		}
	}
	t.Fatalf("schema %q has no inline `required: [...]` list; if the list was reformatted, update this test", schemaName)
	return nil
}

// sortedKeys returns the response keys, sorted, for stable comparisons.
func sortedKeys(m map[string]any) []string {
	out := make([]string, 0, len(m))
	for k := range m {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

// assertResponseKeys asserts that the response data object carries exactly the
// expected keys.
func assertResponseKeys(t *testing.T, payload map[string]any, expected []string, label string) {
	t.Helper()
	data, ok := payload["data"].(map[string]any)
	if !ok {
		t.Fatalf("%s: response has no data object: %#v", label, payload)
	}
	sort.Strings(expected)
	if got := sortedKeys(data); strings.Join(got, ",") != strings.Join(expected, ",") {
		t.Fatalf("%s: response keys = %v, want exactly %v", label, got, expected)
	}
}

// TestCreateStationResponseMatchesSchema is the closure of the D-5 finding: the
// create response must match the schema it is registered under, which is now the
// dedicated StationCreateResponse that does not require the aggregates a create
// cannot produce.
func TestCreateStationResponseMatchesSchema(t *testing.T) {
	want := []string{"address", "code", "id", "latitudeE6", "longitudeE6", "name", "status"}

	// The schema and this list must agree, so neither can drift alone.
	if got := schemaRequiredList(t, "StationCreateResponse"); strings.Join(got, ",") != strings.Join(want, ",") {
		t.Fatalf("StationCreateResponse requires %v, want %v", got, want)
	}
	// The aggregates are declared but optional: a write may answer with them (a
	// create does, as zeros, which is true for a station with no chargers) and a
	// write that does not compute them is still compliant. What must not happen is
	// the schema *requiring* them, which is what the dedicated schema fixed.
	spec, err := os.ReadFile(filepath.Clean(specPath))
	if err != nil {
		t.Fatalf("read spec: %v", err)
	}
	block := schemaBlock(t, string(spec), "StationCreateResponse")
	for _, aggregate := range []string{"chargerCount", "idleChargerCount", "minPriceCentPerKwh"} {
		if !strings.Contains(block, aggregate+":") {
			t.Errorf("StationCreateResponse should declare %q as an optional property so a write may include it", aggregate)
		}
	}
	for _, required := range schemaRequiredList(t, "StationCreateResponse") {
		switch required {
		case "chargerCount", "idleChargerCount", "minPriceCentPerKwh":
			t.Errorf("%q must not be required on a write response", required)
		}
	}

	f := newFixture(t, adminIdentity(auth.AdminRoleSuper), true)
	f.store.createResult = StationRecord{ID: 3, Code: "ST-3", Name: "站3", Status: "OPEN"}
	recorder, payload := do(t, f.server.Handler(), http.MethodPost, "/api/v1/admin/stations",
		`{"code":"ST-3","name":"站3","address":"路1","latitudeE6":31230000,"longitudeE6":121470000}`,
		map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != 201 {
		t.Fatalf("status = %d body = %s", recorder.Code, recorder.Body.String())
	}
	assertResponseKeys(t, payload, append(append([]string{}, want...), "chargerCount", "idleChargerCount", "minPriceCentPerKwh"), "POST /admin/stations")

	// A freshly created station has no chargers, so the zeros the create reports
	// are its real state, not placeholders - the test pins that so a later change
	// cannot quietly start reporting something else there.
	data := payload["data"].(map[string]any)
	for _, aggregate := range []string{"chargerCount", "idleChargerCount", "minPriceCentPerKwh"} {
		value, present := data[aggregate]
		if !present {
			continue
		}
		if value.(float64) != 0 {
			t.Fatalf("create response reports %s = %v for a station with no chargers", aggregate, value)
		}
	}
}

// schemaBlock returns the text of one components/schemas entry.
func schemaBlock(t *testing.T, spec, name string) string {
	t.Helper()
	const indent = "    "
	lines := strings.Split(spec, "\n")
	var out []string
	started := false
	for _, line := range lines {
		if line == indent+name+":" {
			started = true
			continue
		}
		if started {
			if strings.HasPrefix(line, indent) && !strings.HasPrefix(line, indent+" ") && line != "" {
				break
			}
			out = append(out, line)
		}
	}
	if len(out) == 0 {
		t.Fatalf("schema %q is empty or missing", name)
	}
	return strings.Join(out, "\n")
}

// TestStationStatusResponseMatchesSchema covers the status endpoint, which is
// registered against the same schema as the create.
func TestStationStatusResponseMatchesSchema(t *testing.T) {
	want := []string{"address", "code", "id", "latitudeE6", "longitudeE6", "name", "status"}

	f := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	f.store.stationStatus = StationRecord{ID: 9, Code: "ST-9", Name: "站9", Status: "CLOSED"}
	recorder, payload := do(t, f.server.Handler(), http.MethodPut, "/api/v1/admin/stations/9/status",
		`{"status":"CLOSED"}`, nil)
	if recorder.Code != 200 {
		t.Fatalf("status = %d body = %s", recorder.Code, recorder.Body.String())
	}
	assertResponseKeys(t, payload, want, "PUT /admin/stations/{id}/status")
}

// TestChargerStatusResponseMatchesSchema is the charger half.
func TestChargerStatusResponseMatchesSchema(t *testing.T) {
	want := []string{"chargerCode", "chargerId", "status"}
	if got := schemaRequiredList(t, "ChargerStatusRecord"); strings.Join(got, ",") != strings.Join(want, ",") {
		t.Fatalf("ChargerStatusRecord requires %v, want %v", got, want)
	}

	f := newFixture(t, adminIdentity(auth.AdminRoleSuper), true)
	f.store.chargerStatus = ChargerStatusRecord{ChargerID: 5, ChargerCode: "C01", Status: "DISABLED"}
	recorder, payload := do(t, f.server.Handler(), http.MethodPut, "/api/v1/admin/chargers/5/status",
		`{"status":"DISABLED"}`, nil)
	if recorder.Code != 200 {
		t.Fatalf("status = %d body = %s", recorder.Code, recorder.Body.String())
	}
	assertResponseKeys(t, payload, want, "PUT /api/v1/admin/chargers/{id}/status")
}

// TestDeviceCommandResponseMatchesSchema checks the command lookup against its
// schema, including that the response carries commandId and nothing named
// commandNo.
func TestDeviceCommandResponseMatchesSchema(t *testing.T) {
	// orderNo is optional in the schema, so it is not in the required list.
	want := []string{"action", "applied", "chargerId", "commandId", "recordedAt", "result"}
	if got := schemaRequiredList(t, "DeviceCommand"); strings.Join(got, ",") != strings.Join(want, ",") {
		t.Fatalf("DeviceCommand requires %v, want %v", got, want)
	}

	f := newFixture(t, adminIdentity(auth.AdminRoleAuditor), true)
	f.store.command = DeviceCommand{
		CommandID: "CMD20260915000000deadbeef", ChargerID: 5, OrderNo: "ORD20260914120000aaaa",
		Action: "START", Result: "COMPLETED", Applied: true, RecordedAt: "2026-09-15T00:00:00Z",
	}
	recorder, payload := do(t, f.server.Handler(), http.MethodGet,
		"/api/v1/admin/device-commands/CMD20260915000000deadbeef", "", nil)
	if recorder.Code != 200 {
		t.Fatalf("status = %d body = %s", recorder.Code, recorder.Body.String())
	}
	assertResponseKeys(t, payload, append(want, "orderNo"), "GET /admin/device-commands/{commandId}")
}
