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

// Station editing and the fleet-wide tariff: what each endpoint accepts, what
// it refuses before doing any work, and what the console receives back.

const profilePath = "/api/v1/admin/stations/7"

// —— station editing ——

func TestUpdateStationRequiresAWriterRole(t *testing.T) {
	body := `{"name":"新名字","address":"新地址","latitudeE6":30000000,"longitudeE6":104000000}`

	anonymous := newFixture(t, auth.Identity{}, false)
	if recorder, _ := do(t, anonymous.server.Handler(), http.MethodPut, profilePath, body, nil); recorder.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want 401 without a session", recorder.Code)
	}

	// An auditor may read everything and change nothing: this is a write.
	auditor := newFixture(t, adminIdentity(auth.AdminRoleAuditor), true)
	recorder, payload := do(t, auditor.server.Handler(), http.MethodPut, profilePath, body, nil)
	if recorder.Code != http.StatusForbidden || payload["code"] != float64(httpapi.CodeForbidden) {
		t.Fatalf("auditor edit: status = %d code = %v", recorder.Code, payload["code"])
	}
	if len(auditor.store.updateCommands) != 0 {
		t.Fatal("a refused edit reached the store")
	}

	// A user session is not an administrator session at all.
	user := newFixture(t, auth.Identity{ID: 4, Role: auth.RoleUser}, true)
	if recorder, _ := do(t, user.server.Handler(), http.MethodPut, profilePath, body, nil); recorder.Code != http.StatusForbidden {
		t.Fatalf("user edit: status = %d, want 403", recorder.Code)
	}
}

func TestUpdateStationIsAReadOnlyRoleAwayFromTheListRoute(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	// The collection route is POST-only for creation; a PUT there must not be
	// silently treated as an edit of "the" station.
	recorder, _ := do(t, f.server.Handler(), http.MethodPut, "/api/v1/admin/stations", `{}`, nil)
	if recorder.Code != http.StatusMethodNotAllowed {
		t.Fatalf("status = %d, want 405 on the collection route", recorder.Code)
	}
	// And the profile route only answers PUT.
	recorder, _ = do(t, f.server.Handler(), http.MethodGet, profilePath, "", nil)
	if recorder.Code != http.StatusMethodNotAllowed {
		t.Fatalf("status = %d, want 405 for a GET", recorder.Code)
	}
	if allow := recorder.Header().Get("Allow"); allow != http.MethodPut {
		t.Fatalf("Allow = %q, want PUT", allow)
	}
}

func TestUpdateStationValidatesTheProfile(t *testing.T) {
	cases := map[string]string{
		"empty name":         `{"name":"","address":"地址","latitudeE6":30000000,"longitudeE6":104000000}`,
		"blank name":         `{"name":"   ","address":"地址","latitudeE6":30000000,"longitudeE6":104000000}`,
		"missing address":    `{"name":"站","address":"","latitudeE6":30000000,"longitudeE6":104000000}`,
		"name too long":      `{"name":"` + strings.Repeat("站", 101) + `","address":"地址","latitudeE6":30000000,"longitudeE6":104000000}`,
		"address too long":   `{"name":"站","address":"` + strings.Repeat("地", 256) + `","latitudeE6":30000000,"longitudeE6":104000000}`,
		"latitude too large": `{"name":"站","address":"地址","latitudeE6":91000000,"longitudeE6":104000000}`,
		"longitude too big":  `{"name":"站","address":"地址","latitudeE6":30000000,"longitudeE6":181000000}`,
		"not json":           `{`,
	}
	for name, body := range cases {
		t.Run(name, func(t *testing.T) {
			f := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
			recorder, payload := do(t, f.server.Handler(), http.MethodPut, profilePath, body, nil)
			if recorder.Code != http.StatusBadRequest {
				t.Fatalf("status = %d, want 400 (%s)", recorder.Code, recorder.Body.String())
			}
			if payload["code"] != float64(httpapi.CodeInvalidArgument) {
				t.Fatalf("code = %v, want INVALID_ARGUMENT", payload["code"])
			}
			if len(f.store.updateCommands) != 0 {
				t.Fatal("a refused profile reached the store")
			}
		})
	}
}

// The four editable fields are the only ones the endpoint touches: a body that
// carries a code or a status must not change either.
func TestUpdateStationIgnoresWhatItDoesNotEdit(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	f.store.updateResult = StationRecord{ID: 7, Code: "ST-KEEP", Name: "新名字", Address: "新地址",
		LatitudeE6: 30000000, LongitudeE6: 104000000, Status: "OPEN"}

	body := `{"name":"新名字","address":"新地址","latitudeE6":30000000,"longitudeE6":104000000,
	          "code":"ST-CHANGED","status":"DISABLED"}`
	recorder, payload := do(t, f.server.Handler(), http.MethodPut, profilePath, body, nil)
	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d (%s)", recorder.Code, recorder.Body.String())
	}
	if len(f.store.updateCommands) != 1 {
		t.Fatalf("commands = %d, want one edit", len(f.store.updateCommands))
	}
	command := f.store.updateCommands[0]
	if command.StationID != 7 || command.AdminID != 2 {
		t.Fatalf("command = %+v, want the path's station and the session's administrator", command)
	}
	if command.Name != "新名字" || command.Address != "新地址" {
		t.Fatalf("command = %+v", command)
	}
	// The record the console receives still carries the code and the status it
	// never asked to change.
	data, _ := payload["data"].(map[string]any)
	if data["code"] != "ST-KEEP" || data["status"] != "OPEN" {
		t.Fatalf("data = %#v, want the code and status untouched", data)
	}
}

// Whitespace around a name is not part of the name.
func TestUpdateStationTrimsTheName(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	body := `{"name":"  中关村站  ","address":"  地址  ","latitudeE6":30000000,"longitudeE6":104000000}`
	if recorder, _ := do(t, f.server.Handler(), http.MethodPut, profilePath, body, nil); recorder.Code != http.StatusOK {
		t.Fatalf("status = %d", recorder.Code)
	}
	if f.store.updateCommands[0].Name != "中关村站" || f.store.updateCommands[0].Address != "地址" {
		t.Fatalf("command = %+v, want the values trimmed", f.store.updateCommands[0])
	}
}

func TestUpdateStationReportsAMissingStation(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	f.store.updateErr = ErrStationNotFound
	body := `{"name":"站","address":"地址","latitudeE6":30000000,"longitudeE6":104000000}`
	recorder, payload := do(t, f.server.Handler(), http.MethodPut, profilePath, body, nil)
	if recorder.Code != http.StatusNotFound || payload["code"] != float64(httpapi.CodeResourceNotFound) {
		t.Fatalf("status = %d code = %v, want 404 NOT_FOUND", recorder.Code, payload["code"])
	}
}

func TestUpdateStationRejectsANonNumericStationID(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	body := `{"name":"站","address":"地址","latitudeE6":30000000,"longitudeE6":104000000}`
	recorder, _ := do(t, f.server.Handler(), http.MethodPut, "/api/v1/admin/stations/abc", body, nil)
	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", recorder.Code)
	}
	if len(f.store.updateCommands) != 0 {
		t.Fatal("a malformed id reached the store")
	}
}

// The service also refuses on its own, so a caller that bypasses the handler
// cannot write a station the platform would not have created.
func TestUpdateStationServiceBounds(t *testing.T) {
	service, err := NewService(&fakeStore{})
	if err != nil {
		t.Fatalf("NewService() error = %v", err)
	}
	valid := UpdateStationCommand{AdminID: 1, StationID: 1, Name: "站", Address: "地址",
		LatitudeE6: 30000000, LongitudeE6: 104000000}
	if _, err := service.UpdateStation(context.Background(), valid); err != nil {
		t.Fatalf("a valid edit was refused: %v", err)
	}

	cases := map[string]UpdateStationCommand{
		"no actor":     {AdminID: 0, StationID: 1, Name: "站", Address: "地址"},
		"no station":   {AdminID: 1, StationID: 0, Name: "站", Address: "地址"},
		"no name":      {AdminID: 1, StationID: 1, Name: " ", Address: "地址"},
		"bad latitude": {AdminID: 1, StationID: 1, Name: "站", Address: "地址", LatitudeE6: 90000001},
	}
	for name, command := range cases {
		t.Run(name, func(t *testing.T) {
			if _, err := service.UpdateStation(context.Background(), command); err == nil {
				t.Fatal("expected the command to be refused")
			}
		})
	}
}

// —— the fleet-wide tariff ——

func TestGlobalTariffIsReadableByEveryAdminRole(t *testing.T) {
	for _, role := range []string{auth.AdminRoleOperator, auth.AdminRoleAuditor} {
		t.Run(role, func(t *testing.T) {
			f := newFixture(t, adminIdentity(role), true)
			f.store.globalTariff = []GlobalTariff{
				{ElectricityPriceCent: 85, ServicePriceCent: 50, ChargerCount: 3},
			}
			recorder, payload := do(t, f.server.Handler(), http.MethodGet, AdminTariffsPattern, "", nil)
			if recorder.Code != http.StatusOK {
				t.Fatalf("status = %d, want 200 for %s (%s)", recorder.Code, role, recorder.Body.String())
			}
			data, _ := payload["data"].(map[string]any)
			if data["chargerCount"] != float64(3) {
				t.Fatalf("data = %#v", data)
			}
		})
	}

	anonymous := newFixture(t, auth.Identity{}, false)
	if recorder, _ := do(t, anonymous.server.Handler(), http.MethodGet, AdminTariffsPattern, "", nil); recorder.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want 401 without a session", recorder.Code)
	}
}

// Changing what every charge costs is a write, and an auditor is not allowed to
// make it.
func TestUpdateGlobalTariffRequiresAWriterRole(t *testing.T) {
	body := `{"electricityPriceCentPerKwh":100,"servicePriceCentPerKwh":50,"reason":"统一费率"}`
	auditor := newFixture(t, adminIdentity(auth.AdminRoleAuditor), true)
	recorder, payload := do(t, auditor.server.Handler(), http.MethodPut, AdminTariffsPattern, body, nil)
	if recorder.Code != http.StatusForbidden || payload["code"] != float64(httpapi.CodeForbidden) {
		t.Fatalf("auditor: status = %d code = %v", recorder.Code, payload["code"])
	}
	if len(auditor.store.globalUpdates) != 0 {
		t.Fatal("a refused change reached the store")
	}
}

func TestGlobalTariffRoutesRejectOtherMethods(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	recorder, _ := do(t, f.server.Handler(), http.MethodPost, AdminTariffsPattern, `{}`, nil)
	if recorder.Code != http.StatusMethodNotAllowed {
		t.Fatalf("status = %d, want 405", recorder.Code)
	}
	if allow := recorder.Header().Get("Allow"); allow != "GET, PUT" {
		t.Fatalf("Allow = %q, want GET, PUT", allow)
	}
}

func TestUpdateGlobalTariffValidatesItsInput(t *testing.T) {
	cases := map[string]string{
		"negative electricity": `{"electricityPriceCentPerKwh":-1,"servicePriceCentPerKwh":50,"reason":"统一费率"}`,
		"negative service":     `{"electricityPriceCentPerKwh":100,"servicePriceCentPerKwh":-1,"reason":"统一费率"}`,
		"no reason":            `{"electricityPriceCentPerKwh":100,"servicePriceCentPerKwh":50}`,
		"reason too short":     `{"electricityPriceCentPerKwh":100,"servicePriceCentPerKwh":50,"reason":"x"}`,
		"reason too long":      `{"electricityPriceCentPerKwh":100,"servicePriceCentPerKwh":50,"reason":"` + strings.Repeat("因", 201) + `"}`,
		"partial off-peak":     `{"electricityPriceCentPerKwh":100,"servicePriceCentPerKwh":50,"reason":"统一费率","offPeakStartHour":23}`,
		"off-peak price only":  `{"electricityPriceCentPerKwh":100,"servicePriceCentPerKwh":50,"reason":"统一费率","offPeakElectricityPriceCentPerKwh":60}`,
		"same window ends":     `{"electricityPriceCentPerKwh":100,"servicePriceCentPerKwh":50,"reason":"统一费率","offPeakElectricityPriceCentPerKwh":60,"offPeakStartHour":7,"offPeakEndHour":7}`,
		"hour out of range":    `{"electricityPriceCentPerKwh":100,"servicePriceCentPerKwh":50,"reason":"统一费率","offPeakElectricityPriceCentPerKwh":60,"offPeakStartHour":24,"offPeakEndHour":7}`,
		"negative off-peak":    `{"electricityPriceCentPerKwh":100,"servicePriceCentPerKwh":50,"reason":"统一费率","offPeakElectricityPriceCentPerKwh":-1,"offPeakStartHour":23,"offPeakEndHour":7}`,
	}
	for name, body := range cases {
		t.Run(name, func(t *testing.T) {
			f := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
			recorder, _ := do(t, f.server.Handler(), http.MethodPut, AdminTariffsPattern, body, nil)
			if recorder.Code != http.StatusBadRequest {
				t.Fatalf("status = %d, want 400 (%s)", recorder.Code, recorder.Body.String())
			}
			if len(f.store.globalUpdates) != 0 {
				t.Fatal("a refused change reached the store")
			}
		})
	}
}

// The whole tariff travels, including the decision to drop the time-of-use
// window: after the call every charger holds exactly what was sent.
func TestUpdateGlobalTariffSendsTheCompleteTariff(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	f.store.globalResult = GlobalTariffResult{AffectedChargers: 12, PreviousConfigurations: 3,
		ElectricityPriceCent: 100, ServicePriceCent: 50}

	body := `{"electricityPriceCentPerKwh":100,"servicePriceCentPerKwh":50,"reason":"统一费率"}`
	recorder, payload := do(t, f.server.Handler(), http.MethodPut, AdminTariffsPattern, body, nil)
	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d (%s)", recorder.Code, recorder.Body.String())
	}
	update := f.store.globalUpdates[0]
	if update.AdminID != 2 || update.Reason != "统一费率" {
		t.Fatalf("update = %+v, want the session's administrator and the reason", update)
	}
	if update.OffPeakPriceCent != nil || update.OffPeakStartHour != nil || update.OffPeakEndHour != nil {
		t.Fatalf("update = %+v, want an absent off-peak window to stay absent", update)
	}
	data, _ := payload["data"].(map[string]any)
	if data["affectedChargers"] != float64(12) || data["previousConfigurations"] != float64(3) {
		t.Fatalf("data = %#v", data)
	}
}

func TestUpdateGlobalTariffAcceptsATimeOfUseWindow(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	f.store.globalResult = GlobalTariffResult{AffectedChargers: 4}
	body := `{"electricityPriceCentPerKwh":100,"servicePriceCentPerKwh":50,"reason":"峰谷分时",
	          "offPeakElectricityPriceCentPerKwh":60,"offPeakStartHour":23,"offPeakEndHour":7}`
	if recorder, _ := do(t, f.server.Handler(), http.MethodPut, AdminTariffsPattern, body, nil); recorder.Code != http.StatusOK {
		t.Fatalf("status = %d (%s)", recorder.Code, recorder.Body.String())
	}
	update := f.store.globalUpdates[0]
	if update.OffPeakPriceCent == nil || *update.OffPeakPriceCent != 60 ||
		update.OffPeakStartHour == nil || *update.OffPeakStartHour != 23 ||
		update.OffPeakEndHour == nil || *update.OffPeakEndHour != 7 {
		t.Fatalf("update = %+v, want the window carried through", update)
	}
}

// —— the fleet picture ——

func TestGlobalTariffViewSummarisesTheFleet(t *testing.T) {
	peak := int64(60)
	view := summarizeTariffs([]GlobalTariff{
		{ElectricityPriceCent: 120, ServicePriceCent: 50, ChargerCount: 5},
		{ElectricityPriceCent: 85, ServicePriceCent: 30, OffPeakPriceCent: &peak, ChargerCount: 2},
		{ElectricityPriceCent: 100, ServicePriceCent: 40, ChargerCount: 1},
	})

	if view.ChargerCount != 8 || view.ConfigurationCount != 3 {
		t.Fatalf("view = %+v", view)
	}
	if view.MinElectricityPriceCent != 85 || view.MaxElectricityPriceCent != 120 {
		t.Fatalf("electricity range = %d..%d", view.MinElectricityPriceCent, view.MaxElectricityPriceCent)
	}
	if view.MinServicePriceCent != 30 || view.MaxServicePriceCent != 50 {
		t.Fatalf("service range = %d..%d", view.MinServicePriceCent, view.MaxServicePriceCent)
	}
	if view.OffPeakChargerCount != 2 || view.FlatChargerCount != 6 {
		t.Fatalf("view = %+v, want the time-of-use split", view)
	}
}

// An empty fleet reports zeroes and an empty list, not a nil that a client has
// to special-case.
func TestGlobalTariffViewOfAnEmptyFleet(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	f.store.globalTariff = nil
	recorder, payload := do(t, f.server.Handler(), http.MethodGet, AdminTariffsPattern, "", nil)
	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d", recorder.Code)
	}
	data, _ := payload["data"].(map[string]any)
	if data["chargerCount"] != float64(0) || data["configurationCount"] != float64(0) {
		t.Fatalf("data = %#v", data)
	}
	configurations, isList := data["configurations"].([]any)
	if !isList {
		t.Fatalf("configurations = %#v, want a list", data["configurations"])
	}
	if len(configurations) != 0 {
		t.Fatalf("configurations = %#v, want none", configurations)
	}
}

// A store failure is a service problem: the console must retry rather than fix
// its request.
func TestGlobalTariffReportsAStoreFailureAsUnavailable(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	f.store.globalTariffErr = errors.New("the database is unreachable")
	recorder, _ := do(t, f.server.Handler(), http.MethodGet, AdminTariffsPattern, "", nil)
	if recorder.Code != http.StatusServiceUnavailable {
		t.Fatalf("status = %d, want 503", recorder.Code)
	}
}
