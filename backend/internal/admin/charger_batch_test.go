package admin

import (
	"context"
	"errors"
	"net/http"
	"strings"
	"testing"

	"github.com/heguangV/charging-station-platform/backend/internal/auth"
	"github.com/heguangV/charging-station-platform/backend/internal/httpapi"
)

// Batch device creation: what the endpoint accepts, what it refuses before
// opening a transaction, and what the console receives.

const batchPath = "/api/v1/admin/chargers/batch"

func batchBody(chargers string) string {
	return `{"stationId":7,"chargers":[` + chargers + `]}`
}

func oneCharger(code, connector string, power int64) string {
	return `{"code":"` + code + `","connectorType":"` + connector + `","powerWatt":` + itoa(power) + `}`
}

func itoa(value int64) string {
	if value == 0 {
		return "0"
	}
	digits := ""
	for value > 0 {
		digits = string(rune('0'+value%10)) + digits
		value /= 10
	}
	return digits
}

// —— authorisation ——

// Creating devices is a write, and it needs an idempotency key like every other
// write on this boundary.
func TestCreateChargersRequiresAWriterWithAKey(t *testing.T) {
	body := batchBody(oneCharger("A-01", "DC", 120000))

	anonymous := newFixture(t, auth.Identity{}, false)
	if recorder, _ := do(t, anonymous.server.Handler(), http.MethodPost, batchPath, body,
		map[string]string{"Idempotency-Key": idemKey}); recorder.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want 401 without a session", recorder.Code)
	}

	auditor := newFixture(t, adminIdentity(auth.AdminRoleAuditor), true)
	recorder, payload := do(t, auditor.server.Handler(), http.MethodPost, batchPath, body,
		map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusForbidden || payload["code"] != float64(httpapi.CodeForbidden) {
		t.Fatalf("auditor: status = %d code = %v", recorder.Code, payload["code"])
	}
	if len(auditor.store.batchCommands) != 0 {
		t.Fatal("a refused batch reached the store")
	}

	operator := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	recorder, _ = do(t, operator.server.Handler(), http.MethodPost, batchPath, body, nil)
	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400 without an idempotency key", recorder.Code)
	}
	if len(operator.store.batchCommands) != 0 {
		t.Fatal("a request without a key reached the store")
	}
}

func TestCreateChargersRejectsOtherMethods(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	recorder, _ := do(t, f.server.Handler(), http.MethodGet, batchPath, "", nil)
	if recorder.Code != http.StatusMethodNotAllowed {
		t.Fatalf("status = %d, want 405", recorder.Code)
	}
	if allow := recorder.Header().Get("Allow"); allow != http.MethodPost {
		t.Fatalf("Allow = %q, want POST", allow)
	}
}

// —— validation ——

// Every refusal below happens before the store is called, so a batch that cannot
// succeed never opens a transaction.
func TestCreateChargersRefusesAnInvalidBatch(t *testing.T) {
	cases := map[string]string{
		"no chargers":         `{"stationId":7,"chargers":[]}`,
		"chargers missing":    `{"stationId":7}`,
		"station zero":        `{"stationId":0,"chargers":[` + oneCharger("A-01", "DC", 120000) + `]}`,
		"station missing":     `{"chargers":[` + oneCharger("A-01", "DC", 120000) + `]}`,
		"empty code":          batchBody(oneCharger("", "DC", 120000)),
		"blank code":          batchBody(oneCharger("   ", "DC", 120000)),
		"code too long":       batchBody(oneCharger(strings.Repeat("C", 33), "DC", 120000)),
		"code with newline":   batchBody(oneCharger("A\n-01", "DC", 120000)),
		"unknown connector":   batchBody(oneCharger("A-01", "AC/DC", 120000)),
		"lowercase connector": batchBody(oneCharger("A-01", "dc", 120000)),
		"zero power":          batchBody(oneCharger("A-01", "DC", 0)),
		"negative power":      batchBody(oneCharger("A-01", "DC", -1)),
		"power too high":      batchBody(oneCharger("A-01", "DC", 1_000_001)),
		"not json":            `{`,
	}
	for name, body := range cases {
		t.Run(name, func(t *testing.T) {
			f := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
			recorder, payload := do(t, f.server.Handler(), http.MethodPost, batchPath, body,
				map[string]string{"Idempotency-Key": idemKey})
			if recorder.Code != http.StatusBadRequest {
				t.Fatalf("status = %d, want 400 (%s)", recorder.Code, recorder.Body.String())
			}
			if payload["code"] != float64(httpapi.CodeInvalidArgument) {
				t.Fatalf("code = %v, want INVALID_ARGUMENT", payload["code"])
			}
			if len(f.store.batchCommands) != 0 {
				t.Fatal("a refused batch reached the store")
			}
		})
	}
}

// A batch of exactly the maximum is accepted: the bound is a bound.
func TestCreateChargersAcceptsTheMaximumBatch(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	entries := make([]string, 0, MaxBatchChargers)
	for index := 0; index < MaxBatchChargers; index++ {
		entries = append(entries, oneCharger("A-"+itoa(int64(index)), "DC", 120000))
	}
	recorder, _ := do(t, f.server.Handler(), http.MethodPost, batchPath, batchBody(strings.Join(entries, ",")),
		map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusCreated {
		t.Fatalf("status = %d, want 201 (%s)", recorder.Code, recorder.Body.String())
	}
	if len(f.store.batchCommands) != 1 || len(f.store.batchCommands[0].Chargers) != MaxBatchChargers {
		t.Fatalf("commands = %+v", f.store.batchCommands)
	}
}

func TestCreateChargersRefusesAnOversizedBatch(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	entries := make([]string, 0, MaxBatchChargers+1)
	for index := 0; index <= MaxBatchChargers; index++ {
		entries = append(entries, oneCharger("A-"+itoa(int64(index)), "DC", 120000))
	}
	recorder, payload := do(t, f.server.Handler(), http.MethodPost, batchPath, batchBody(strings.Join(entries, ",")),
		map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", recorder.Code)
	}
	if !strings.Contains(payload["message"].(string), "1 to 100") {
		t.Fatalf("message = %q, want the batch bound", payload["message"])
	}
	if len(f.store.batchCommands) != 0 {
		t.Fatal("an oversized batch reached the store")
	}
}

// A code repeated inside one request is a duplicate, and it is caught here so
// the answer names it rather than leaving the operator to compare two rows.
func TestCreateChargersRefusesARepeatedCodeInTheRequest(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	body := batchBody(oneCharger("A-01", "DC", 120000) + "," + oneCharger("A-01", "AC", 7000))
	recorder, payload := do(t, f.server.Handler(), http.MethodPost, batchPath, body,
		map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusConflict {
		t.Fatalf("status = %d, want 409 (%s)", recorder.Code, recorder.Body.String())
	}
	if payload["code"] != float64(5) {
		t.Fatalf("code = %v, want ALREADY_EXISTS", payload["code"])
	}
	if !strings.Contains(payload["message"].(string), "A-01") {
		t.Fatalf("message = %q, want it to name the repeated code", payload["message"])
	}
	if len(f.store.batchCommands) != 0 {
		t.Fatal("a refused batch reached the store")
	}
}

// —— the response ——

// The console receives the devices it created, and the count it reports.
func TestCreateChargersReturnsWhatItCreated(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	f.store.batchResult = ChargerBatchResult{
		StationID:    7,
		ChargerCount: 2,
		Created: []ChargerRecord{
			{ID: 11, StationID: 7, Code: "A-01", Type: "DC", PowerWatt: 120000, Status: "IDLE"},
			{ID: 12, StationID: 7, Code: "A-02", Type: "DC", PowerWatt: 120000, Status: "IDLE"},
		},
	}

	body := batchBody(oneCharger("A-01", "DC", 120000) + "," + oneCharger("A-02", "DC", 120000))
	recorder, payload := do(t, f.server.Handler(), http.MethodPost, batchPath, body,
		map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusCreated {
		t.Fatalf("status = %d, want 201 (%s)", recorder.Code, recorder.Body.String())
	}

	command := f.store.batchCommands[0]
	if command.AdminID != 2 || command.StationID != 7 {
		t.Fatalf("command = %+v, want the session's administrator and the path's station", command)
	}
	if command.IdempotencyKey != idemKey || command.RequestHash == "" || command.TraceID == "" {
		t.Fatalf("command = %+v, want the key, the request hash and the trace", command)
	}

	data, _ := payload["data"].(map[string]any)
	if data["chargerCount"] != float64(2) {
		t.Fatalf("data = %#v", data)
	}
	created, isList := data["created"].([]any)
	if !isList || len(created) != 2 {
		t.Fatalf("created = %#v, want the two devices", data["created"])
	}
	first, _ := created[0].(map[string]any)
	for _, key := range []string{"id", "stationId", "code", "type", "powerWatt", "status"} {
		if _, present := first[key]; !present {
			t.Fatalf("a created device is missing %q: %#v", key, first)
		}
	}
}

// A code that already exists at the station comes back from the store as a
// conflict, not as an internal error.
func TestCreateChargersReportsAnExistingCodeAsAConflict(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	f.store.batchErr = ErrDuplicateChargerCode
	recorder, payload := do(t, f.server.Handler(), http.MethodPost, batchPath,
		batchBody(oneCharger("A-01", "DC", 120000)),
		map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusConflict || payload["code"] != float64(5) {
		t.Fatalf("status = %d code = %v, want 409 ALREADY_EXISTS", recorder.Code, payload["code"])
	}
}

func TestCreateChargersReportsAMissingStation(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	f.store.batchErr = ErrStationNotFound
	recorder, payload := do(t, f.server.Handler(), http.MethodPost, batchPath,
		batchBody(oneCharger("A-01", "DC", 120000)),
		map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusNotFound || payload["code"] != float64(httpapi.CodeResourceNotFound) {
		t.Fatalf("status = %d code = %v, want 404 NOT_FOUND", recorder.Code, payload["code"])
	}
}

// A store failure is a service problem: the console must retry rather than
// change its request.
func TestCreateChargersReportsAStoreFailureAsUnavailable(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	f.store.batchErr = errors.New("the database is unreachable")
	recorder, _ := do(t, f.server.Handler(), http.MethodPost, batchPath,
		batchBody(oneCharger("A-01", "DC", 120000)),
		map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusServiceUnavailable {
		t.Fatalf("status = %d, want 503", recorder.Code)
	}
}

// —— the service's own bounds ——
//
// The handler validates too, but the service must refuse on its own: nothing
// guarantees a future caller goes through the handler.
func TestCreateChargersServiceBounds(t *testing.T) {
	service, err := NewService(&fakeStore{})
	if err != nil {
		t.Fatalf("NewService() error = %v", err)
	}
	valid := CreateChargersCommand{
		AdminID: 1, StationID: 7,
		Chargers: []ChargerDraft{{Code: "A-01", ConnectorType: "DC", PowerWatt: 120000}},
	}
	if _, err := service.CreateChargers(context.Background(), valid); err != nil {
		t.Fatalf("a valid batch was refused: %v", err)
	}

	cases := map[string]CreateChargersCommand{
		"no actor":    {AdminID: 0, StationID: 7, Chargers: valid.Chargers},
		"no station":  {AdminID: 1, StationID: 0, Chargers: valid.Chargers},
		"empty batch": {AdminID: 1, StationID: 7},
		"bad connector": {AdminID: 1, StationID: 7,
			Chargers: []ChargerDraft{{Code: "A-01", ConnectorType: "ac", PowerWatt: 1}}},
		"bad power": {AdminID: 1, StationID: 7,
			Chargers: []ChargerDraft{{Code: "A-01", ConnectorType: "AC", PowerWatt: 0}}},
	}
	for name, command := range cases {
		t.Run(name, func(t *testing.T) {
			if _, err := service.CreateChargers(context.Background(), command); err == nil {
				t.Fatal("expected the batch to be refused")
			}
		})
	}
}

// The codes reach the store trimmed, so a batch typed with a stray space does
// not create a device whose code can never be typed again.
func TestCreateChargersTrimsTheCodes(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	body := batchBody(oneCharger("  A-01  ", "DC", 120000))
	if recorder, _ := do(t, f.server.Handler(), http.MethodPost, batchPath, body,
		map[string]string{"Idempotency-Key": idemKey}); recorder.Code != http.StatusCreated {
		t.Fatalf("status = %d", recorder.Code)
	}
	if code := f.store.batchCommands[0].Chargers[0].Code; code != "A-01" {
		t.Fatalf("code = %q, want it trimmed", code)
	}
}
