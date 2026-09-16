package admin

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/heguangV/charging-station-platform/backend/internal/auth"
	"github.com/heguangV/charging-station-platform/backend/internal/config"
	"github.com/heguangV/charging-station-platform/backend/internal/httpapi"
	"github.com/heguangV/charging-station-platform/backend/internal/order"
	"github.com/heguangV/charging-station-platform/backend/internal/station"
	"github.com/heguangV/charging-station-platform/backend/internal/wallet"
)

// fakeStore records admin commands and returns canned results.
type fakeStore struct {
	orderFilter          AdminOrderFilter
	orderFilterSet       bool
	command              DeviceCommand
	commandErr           error
	commandID            string
	commandLookup        bool
	stationStatus        StationRecord
	stationStatusErr     error
	stationStatusCommand ChangeStationStatusCommand
	stationStatusCalled  bool
	chargerStatus        ChargerStatusRecord
	chargerStatusErr     error
	chargerStatusCommand ChangeChargerStatusCommand
	chargerStatusCalled  bool
	createResult         StationRecord
	createErr            error
	stations             StationPage
	restartCmd           Command
	restartErr           error
	users                UserPage
	orders               OrderPage
	chargers             ChargerPage
	restarts             []RestartCommand
	tariffErr            error
	releaseErr           error

	// Station editing and the fleet tariff.
	updateResult    StationRecord
	updateErr       error
	updateCommands  []UpdateStationCommand
	globalTariff    []GlobalTariff
	globalTariffErr error
	globalResult    GlobalTariffResult
	globalErr       error
	globalUpdates   []GlobalTariffUpdate
	batchResult     ChargerBatchResult
	batchErr        error
	batchCommands   []CreateChargersCommand

	// Manual user archiving.
	userResult        UserRecord
	userErr           error
	userCommands      []CreateUserCommand
	userBatchResult   UserBatchResult
	userBatchErr      error
	userBatchCommands []CreateUsersCommand

	// The statistics reads. Their canned results are separate from the page
	// fixtures because an aggregate is not a page: a test that configured one
	// would otherwise silently configure the other.
	revenueTotals  RevenueTotals
	revenueBuckets []RevenueBucket
	chargerCounts  ChargerCounts
	fleetCounts    FleetCounts
	statsErr       error
	revenueQueries []RevenueQuery
	stationFilters []int64
}

func (f *fakeStore) CreateStation(context.Context, CreateStationCommand) (StationRecord, error) {
	return f.createResult, f.createErr
}

func (f *fakeStore) ListStations(context.Context, int64, int64, string) (StationPage, error) {
	return f.stations, nil
}

func (f *fakeStore) ListChargers(context.Context, station.ChargerFilter) (ChargerPage, error) {
	return f.chargers, nil
}

func (f *fakeStore) ListUsers(context.Context, UserFilter) (UserPage, error) {
	return f.users, nil
}

func (f *fakeStore) ListOrders(_ context.Context, filter AdminOrderFilter) (OrderPage, error) {
	f.orderFilter = filter
	f.orderFilterSet = true
	return f.orders, nil
}

func (f *fakeStore) ChangeStationStatus(_ context.Context, command ChangeStationStatusCommand) (StationRecord, error) {
	f.stationStatusCommand = command
	f.stationStatusCalled = true
	if f.stationStatusErr != nil {
		return StationRecord{}, f.stationStatusErr
	}
	return f.stationStatus, nil
}

func (f *fakeStore) ChangeChargerStatus(_ context.Context, command ChangeChargerStatusCommand) (ChargerStatusRecord, error) {
	f.chargerStatusCommand = command
	f.chargerStatusCalled = true
	if f.chargerStatusErr != nil {
		return ChargerStatusRecord{}, f.chargerStatusErr
	}
	return f.chargerStatus, nil
}

func (f *fakeStore) FindDeviceCommand(_ context.Context, commandID string) (DeviceCommand, error) {
	f.commandID = commandID
	f.commandLookup = true
	if f.commandErr != nil {
		return DeviceCommand{}, f.commandErr
	}
	return f.command, nil
}

func (f *fakeStore) RestartCharger(_ context.Context, command RestartCommand) (Command, error) {
	f.restarts = append(f.restarts, command)
	return f.restartCmd, f.restartErr
}

func (f *fakeStore) RevenueTotals(_ context.Context, query RevenueQuery) (RevenueTotals, error) {
	f.revenueQueries = append(f.revenueQueries, query)
	return f.revenueTotals, f.statsErr
}

func (f *fakeStore) RevenueBuckets(_ context.Context, query RevenueQuery) ([]RevenueBucket, error) {
	f.revenueQueries = append(f.revenueQueries, query)
	return f.revenueBuckets, f.statsErr
}

func (f *fakeStore) ChargerCounts(_ context.Context, stationID int64) (ChargerCounts, error) {
	f.stationFilters = append(f.stationFilters, stationID)
	return f.chargerCounts, f.statsErr
}

func (f *fakeStore) FleetCounts(context.Context) (FleetCounts, error) {
	return f.fleetCounts, f.statsErr
}

func (f *fakeStore) CreateChargers(_ context.Context, command CreateChargersCommand) (ChargerBatchResult, error) {
	f.batchCommands = append(f.batchCommands, command)
	return f.batchResult, f.batchErr
}

func (f *fakeStore) CreateUser(_ context.Context, command CreateUserCommand) (UserRecord, error) {
	f.userCommands = append(f.userCommands, command)
	return f.userResult, f.userErr
}

func (f *fakeStore) CreateUsers(_ context.Context, command CreateUsersCommand) (UserBatchResult, error) {
	f.userBatchCommands = append(f.userBatchCommands, command)
	return f.userBatchResult, f.userBatchErr
}

func (f *fakeStore) UpdateStation(_ context.Context, command UpdateStationCommand) (StationRecord, error) {
	f.updateCommands = append(f.updateCommands, command)
	return f.updateResult, f.updateErr
}

func (f *fakeStore) GlobalTariff(context.Context) ([]GlobalTariff, error) {
	return f.globalTariff, f.globalTariffErr
}

func (f *fakeStore) UpdateGlobalTariff(_ context.Context, update GlobalTariffUpdate) (GlobalTariffResult, error) {
	f.globalUpdates = append(f.globalUpdates, update)
	return f.globalResult, f.globalErr
}

func (f *fakeStore) GetTariff(context.Context, int64) (TariffView, error) {
	return TariffView{ChargerID: 5, ElectricityPriceCent: 120, ServicePriceCent: 50}, nil
}

func (f *fakeStore) UpdateTariff(_ context.Context, update TariffUpdate) (TariffView, error) {
	return TariffView{ChargerID: update.ChargerID, ElectricityPriceCent: update.ElectricityPriceCent, ServicePriceCent: update.ServicePriceCent}, f.tariffErr
}

func (f *fakeStore) ForceRelease(_ context.Context, command ForceReleaseCommand) (StationRecordCharger, error) {
	return StationRecordCharger{ChargerID: command.ChargerID, Status: command.TargetStatus}, f.releaseErr
}

func (f *fakeStore) GetUserDetail(context.Context, int64) (UserDetail, error) {
	return UserDetail{ID: 7, Phone: "13800000001", DisplayName: "用户0606", Status: "ACTIVE", BalanceCent: 10000}, nil
}

func (f *fakeStore) ListUserLedger(context.Context, UserLedgerFilter) (LedgerPage, error) {
	return LedgerPage{Items: []LedgerEntry{{TransactionType: wallet.TypeTopUp, AmountCent: 10000}}, Meta: wallet.PageMeta{Total: 1}}, nil
}

func (f *fakeStore) ListAudit(context.Context, AuditFilter) (AuditPage, error) {
	return AuditPage{Items: []AuditEntry{{ID: 1, ActorType: "ADMIN", ActorID: "2", Action: "tariff.update"}}, Meta: PageMeta{Total: 1}}, nil
}

// fakeAuthProvider mirrors auth.Handlers middleware with role enforcement.
type fakeAuthProvider struct {
	identity auth.Identity
	found    bool
}

func (f fakeAuthProvider) RequireRole(role string, next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if !f.found {
			httpapi.WriteError(w, r, http.StatusUnauthorized, httpapi.CodeUnauthorized, "session is missing or expired", nil)
			return
		}
		if !auth.Authorize(f.identity, role) {
			httpapi.WriteError(w, r, http.StatusForbidden, httpapi.CodeForbidden, "insufficient permission", nil)
			return
		}
		next(w, r.WithContext(auth.WithIdentity(r.Context(), f.identity)))
	}
}

func (f fakeAuthProvider) RequireAdminWrite(next http.HandlerFunc) http.HandlerFunc {
	return f.RequireRole(auth.RoleAdmin, func(w http.ResponseWriter, r *http.Request) {
		if !auth.AdminCanWrite(f.identity) {
			httpapi.WriteError(w, r, http.StatusForbidden, httpapi.CodeForbidden, "insufficient permission for administrative writes", nil)
			return
		}
		next(w, r)
	})
}

type fixture struct {
	server *httpapi.Server
	store  *fakeStore
}

func newFixture(t *testing.T, identity auth.Identity, authenticated bool) fixture {
	t.Helper()
	store := &fakeStore{}
	service, err := NewService(store)
	if err != nil {
		t.Fatalf("NewService() error = %v", err)
	}
	handlers, err := NewHandlers(service, fakeAuthProvider{identity: identity, found: authenticated})
	if err != nil {
		t.Fatalf("NewHandlers() error = %v", err)
	}
	server := httpapi.NewServer(config.Config{RequestIDHeader: "X-Request-ID"}, slog.New(slog.NewTextHandler(io.Discard, nil)))
	handlers.Register(server)
	server.SetReady(true)
	return fixture{server: server, store: store}
}

func do(t *testing.T, handler http.Handler, method, path, body string, headers map[string]string) (*httptest.ResponseRecorder, map[string]any) {
	t.Helper()
	var reader io.Reader
	if body != "" {
		reader = strings.NewReader(body)
	}
	request := httptest.NewRequest(method, path, reader)
	if body != "" {
		request.Header.Set("Content-Type", "application/json")
	}
	for key, value := range headers {
		request.Header.Set(key, value)
	}
	recorder := httptest.NewRecorder()
	handler.ServeHTTP(recorder, request)

	var payload map[string]any
	if recorder.Body.Len() > 0 {
		snapshot := recorder.Body.Bytes()
		if err := json.Unmarshal(snapshot, &payload); err != nil {
			t.Fatalf("decode envelope: %v (body %q)", err, recorder.Body.String())
		}
	}
	return recorder, payload
}

const idemKey = "admin-idem-key-000001"

func adminIdentity(role string) auth.Identity {
	return auth.Identity{ID: 2, Role: auth.RoleAdmin, AdminRole: role, Status: auth.StatusActive}
}

func TestAdminCreateStationRequiresWriterRole(t *testing.T) {
	auditor := newFixture(t, adminIdentity(auth.AdminRoleAuditor), true)
	recorder, payload := do(t, auditor.server.Handler(), http.MethodPost, "/api/v1/admin/stations",
		`{"code":"ST-01","name":"站","address":"a","latitudeE6":30000000,"longitudeE6":104000000}`,
		map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusForbidden || payload["code"].(float64) != httpapi.CodeForbidden {
		t.Fatalf("auditor create: status = %d code = %v", recorder.Code, payload["code"])
	}

	operator := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	recorder, payload = do(t, operator.server.Handler(), http.MethodPost, "/api/v1/admin/stations",
		`{"code":"ST-01","name":"站","address":"a","latitudeE6":30000000,"longitudeE6":104000000}`,
		map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusBadRequest {
		// The fixture store returns zero values; validation of the empty
		// record is not asserted here — a 400 would come from the body only.
		t.Logf("operator create status = %d body = %s", recorder.Code, recorder.Body.String())
	}
}

func TestAdminCreateStationValidation(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)

	bad := []string{
		`{"code":"x","name":"站","address":"a","latitudeE6":30000000,"longitudeE6":104000000}`,     // short code
		`{"code":"ST-01","name":"","address":"a","latitudeE6":30000000,"longitudeE6":104000000}`,  // empty name
		`{"code":"ST-01","name":"站","address":"a","latitudeE6":91000000,"longitudeE6":104000000}`, // latitude out of range
		`{"code":"ST-01","name":"站","address":"a","latitudeE6":30000000,"longitudeE6":181000000}`, // longitude out of range
	}
	for _, body := range bad {
		recorder, payload := do(t, f.server.Handler(), http.MethodPost, "/api/v1/admin/stations", body, map[string]string{"Idempotency-Key": idemKey})
		if recorder.Code != http.StatusBadRequest {
			t.Errorf("body %s: status = %d, want 400", body, recorder.Code)
		}
		_ = payload
	}

	// Missing idempotency key
	recorder, _ := do(t, f.server.Handler(), http.MethodPost, "/api/v1/admin/stations",
		`{"code":"ST-01","name":"站","address":"a","latitudeE6":30000000,"longitudeE6":104000000}`, nil)
	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("missing key status = %d", recorder.Code)
	}
}

func TestAdminRestartChargerRoleAndValidation(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleAuditor), true)
	recorder, payload := do(t, f.server.Handler(), http.MethodPost, "/api/v1/admin/chargers/5/restart",
		`{"reason":"例行维护"}`, map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusForbidden || payload["code"].(float64) != httpapi.CodeForbidden {
		t.Fatalf("auditor restart: status = %d code = %v", recorder.Code, payload["code"])
	}

	operator := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	operator.store.restartCmd = Command{CommandID: "CMD20260915000000deadbeef", Status: CommandPending}
	recorder, payload = do(t, operator.server.Handler(), http.MethodPost, "/api/v1/admin/chargers/5/restart",
		`{"reason":"例行维护"}`, map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusAccepted {
		t.Fatalf("operator restart status = %d body = %s", recorder.Code, recorder.Body.String())
	}
	data := payload["data"].(map[string]any)
	if data["commandId"] == "" || data["status"] != CommandPending {
		t.Fatalf("command = %#v", data)
	}
	if operator.store.restarts[0].ChargerID != 5 || operator.store.restarts[0].Reason != "例行维护" {
		t.Fatalf("command = %#v", operator.store.restarts[0])
	}

	// Short reason
	recorder, _ = do(t, operator.server.Handler(), http.MethodPost, "/api/v1/admin/chargers/5/restart",
		`{"reason":"x"}`, map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("short reason status = %d", recorder.Code)
	}
}

func TestAdminListEndpointsReturnPages(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleSuper), true)
	f.store.users = UserPage{Items: []UserSummary{{ID: 7, DisplayName: "用户0606", Status: UserStatusActive, BalanceCent: 10000}},
		Meta: PageMeta{Page: 1, PageSize: 20, Total: 1}}
	f.store.orders = OrderPage{Items: []order.Order{{OrderNo: "ORD20260914120000aaaa", UserID: 7, Status: "COMPLETED", PaymentStatus: "PAID"}},
		Meta: order.PageMeta{Page: 1, PageSize: 20, Total: 1}}

	recorder, payload := do(t, f.server.Handler(), http.MethodGet, "/api/v1/admin/users?status=1", "", nil)
	if recorder.Code != http.StatusOK {
		t.Fatalf("users status = %d body = %s", recorder.Code, recorder.Body.String())
	}
	items := payload["data"].(map[string]any)["items"].([]any)
	if len(items) != 1 || items[0].(map[string]any)["balanceCent"].(float64) != 10000 {
		t.Fatalf("users = %#v", items)
	}

	recorder, payload = do(t, f.server.Handler(), http.MethodGet, "/api/v1/admin/orders?orderNo=ORD2026", "", nil)
	if recorder.Code != http.StatusOK {
		t.Fatalf("orders status = %d", recorder.Code)
	}
	orderItems := payload["data"].(map[string]any)["items"].([]any)
	if orderItems[0].(map[string]any)["paymentStatus"] != "PAID" {
		t.Fatalf("orders = %#v", orderItems)
	}

	// Bad user status query value
	recorder, _ = do(t, f.server.Handler(), http.MethodGet, "/api/v1/admin/users?status=5", "", nil)
	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("bad status query = %d", recorder.Code)
	}
}

func TestAdminRoutesRejectAnonymous(t *testing.T) {
	f := newFixture(t, auth.Identity{}, false)
	for _, path := range []string{"/api/v1/admin/users", "/api/v1/admin/stations", "/api/v1/admin/orders", "/api/v1/admin/chargers"} {
		recorder, _ := do(t, f.server.Handler(), http.MethodGet, path, "", nil)
		if recorder.Code != http.StatusUnauthorized {
			t.Errorf("%s: status = %d, want 401", path, recorder.Code)
		}
	}
}

func TestAdminServiceRejectsNilStore(t *testing.T) {
	if _, err := NewService(nil); err == nil {
		t.Fatal("nil store accepted")
	}
	_ = errors.New
	_ = context.Background
}

func TestTariffUpdateValidationAndAudit(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)

	// Negative prices rejected in the service.
	recorder, _ := do(t, f.server.Handler(), http.MethodPut, "/api/v1/admin/chargers/5/tariff",
		`{"electricityPriceCentPerKwh":-1,"servicePriceCentPerKwh":50}`, map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("negative price status = %d", recorder.Code)
	}

	// Off-peak price without a window rejected.
	recorder, _ = do(t, f.server.Handler(), http.MethodPut, "/api/v1/admin/chargers/5/tariff",
		`{"electricityPriceCentPerKwh":120,"servicePriceCentPerKwh":50,"offPeakElectricityPriceCentPerKwh":60}`,
		map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("off-peak without window status = %d", recorder.Code)
	}

	// Valid update flows through and returns the new tariff.
	recorder, payload := do(t, f.server.Handler(), http.MethodPut, "/api/v1/admin/chargers/5/tariff",
		`{"electricityPriceCentPerKwh":120,"servicePriceCentPerKwh":50,"offPeakElectricityPriceCentPerKwh":60,"offPeakStartHour":23,"offPeakEndHour":7}`,
		map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusOK {
		t.Fatalf("valid tariff status = %d", recorder.Code)
	}
	data := payload["data"].(map[string]any)
	if data["electricityPriceCentPerKwh"].(float64) != 120 || data["servicePriceCentPerKwh"].(float64) != 50 {
		t.Fatalf("tariff = %#v", data)
	}

	// GET returns the view.
	recorder, payload = do(t, f.server.Handler(), http.MethodGet, "/api/v1/admin/chargers/5/tariff", "", nil)
	if recorder.Code != http.StatusOK || payload["data"].(map[string]any)["chargerId"].(float64) != 5 {
		t.Fatalf("get tariff = %#v", payload)
	}
}

func TestForceReleaseRoleAndValidation(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleAuditor), true)
	recorder, payload := do(t, f.server.Handler(), http.MethodPost, "/api/v1/admin/chargers/5/release",
		`{"reason":"违规占位","targetStatus":"IDLE"}`, map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusForbidden || payload["code"].(float64) != httpapi.CodeForbidden {
		t.Fatalf("auditor release: status = %d code = %v", recorder.Code, payload["code"])
	}

	operator := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	recorder, payload = do(t, operator.server.Handler(), http.MethodPost, "/api/v1/admin/chargers/5/release",
		`{"reason":"违规占位","targetStatus":"IDLE"}`, map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusOK {
		t.Fatalf("operator release status = %d body = %s", recorder.Code, recorder.Body.String())
	}
	data := payload["data"].(map[string]any)
	if data["chargerId"].(float64) != 5 || data["status"] != "IDLE" {
		t.Fatalf("release = %#v", data)
	}

	// Charging orders are rejected by the service (BR-11).
	operator.store.releaseErr = ErrInvalidStateTransition
	recorder, payload = do(t, operator.server.Handler(), http.MethodPost, "/api/v1/admin/chargers/5/release",
		`{"reason":"违规占位","targetStatus":"IDLE"}`, map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusConflict || payload["code"].(float64) != 15 {
		t.Fatalf("charging release: status = %d code = %v", recorder.Code, payload["code"])
	}

	// Missing reason
	operator.store.releaseErr = nil
	recorder, _ = do(t, operator.server.Handler(), http.MethodPost, "/api/v1/admin/chargers/5/release",
		`{"reason":"x","targetStatus":"IDLE"}`, map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("short reason status = %d", recorder.Code)
	}

	// Invalid target status
	recorder, _ = do(t, operator.server.Handler(), http.MethodPost, "/api/v1/admin/chargers/5/release",
		`{"reason":"合法原因","targetStatus":"FAULT"}`, map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("bad target status = %d", recorder.Code)
	}
}

func TestUserDetailAndLedgerEndpoints(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleSuper), true)

	recorder, payload := do(t, f.server.Handler(), http.MethodGet, "/api/v1/admin/users/7", "", nil)
	if recorder.Code != http.StatusOK {
		t.Fatalf("user detail status = %d body = %s", recorder.Code, recorder.Body.String())
	}
	data := payload["data"].(map[string]any)
	if data["phone"] != "13800000001" || data["balanceCent"].(float64) != 10000 {
		t.Fatalf("user detail = %#v", data)
	}

	recorder, payload = do(t, f.server.Handler(), http.MethodGet, "/api/v1/admin/users/7/transactions?type=TOP_UP", "", nil)
	if recorder.Code != http.StatusOK {
		t.Fatalf("ledger status = %d", recorder.Code)
	}
	items := payload["data"].(map[string]any)["items"].([]any)
	if items[0].(map[string]any)["transactionType"] != wallet.TypeTopUp {
		t.Fatalf("ledger = %#v", items)
	}
}

func TestAuditQueryEndpoint(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleSuper), true)

	recorder, payload := do(t, f.server.Handler(), http.MethodGet,
		"/api/v1/admin/audit?action=tariff.update&page=1&pageSize=20", "", nil)
	if recorder.Code != http.StatusOK {
		t.Fatalf("audit status = %d", recorder.Code)
	}
	data := payload["data"].(map[string]any)
	items := data["items"].([]any)
	if len(items) != 1 || items[0].(map[string]any)["action"] != "tariff.update" {
		t.Fatalf("audit = %#v", items)
	}
	if data["meta"].(map[string]any)["total"].(float64) != 1 {
		t.Fatalf("audit meta = %#v", data["meta"])
	}
}

// TestAdminOrderListFiltersByUser covers the userId filter the management UI
// needs when it opens a user's detail page: it must reach the store, and a
// malformed or non-positive value must be a 400 rather than a silent full list.
func TestAdminOrderListFiltersByUser(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleSuper), true)
	f.store.orders = OrderPage{Items: []order.Order{{OrderNo: "ORD20260914120000aaaa", UserID: 42, Status: "COMPLETED"}},
		Meta: order.PageMeta{Page: 1, PageSize: 20, Total: 1}}

	recorder, _ := do(t, f.server.Handler(), http.MethodGet, "/api/v1/admin/orders?userId=42", "", nil)
	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d body = %s", recorder.Code, recorder.Body.String())
	}
	if !f.store.orderFilterSet || f.store.orderFilter.UserID != 42 {
		t.Fatalf("order filter = %#v (set=%v), want UserID 42", f.store.orderFilter, f.store.orderFilterSet)
	}

	for _, raw := range []string{"0", "-3", "abc", "4.2"} {
		recorder, payload := do(t, f.server.Handler(), http.MethodGet, "/api/v1/admin/orders?userId="+raw, "", nil)
		if recorder.Code != http.StatusBadRequest {
			t.Fatalf("userId=%s status = %d body = %s", raw, recorder.Code, recorder.Body.String())
		}
		if payload["code"].(float64) != httpapi.CodeInvalidArgument {
			t.Fatalf("userId=%s code = %v", raw, payload["code"])
		}
	}

	// 未提供 userId 时仍然列出全部（0 表示不过滤），并确实到达了服务层。
	f.store.orderFilterSet = false
	recorder, _ = do(t, f.server.Handler(), http.MethodGet, "/api/v1/admin/orders", "", nil)
	if recorder.Code != http.StatusOK || !f.store.orderFilterSet || f.store.orderFilter.UserID != 0 {
		t.Fatalf("no-filter status = %d filter = %#v", recorder.Code, f.store.orderFilter)
	}
}

// TestGetDeviceCommand covers the command-status lookup: an administrator (any
// role, including the read-only AUDITOR) can follow a command to its recorded
// outcome, an unknown id is a 404 rather than an empty 200, a plain user is
// refused, and the id reaches the store unchanged.
func TestGetDeviceCommand(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleAuditor), true)
	f.store.command = DeviceCommand{
		CommandID:  "CMD20260915000000deadbeef",
		ChargerID:  5,
		OrderNo:    "ORD20260914120000aaaa",
		Action:     "START",
		Result:     "SUCCESS",
		Applied:    true,
		RecordedAt: "2026-09-15T00:00:00Z",
	}

	recorder, payload := do(t, f.server.Handler(), http.MethodGet,
		"/api/v1/admin/device-commands/CMD20260915000000deadbeef", "", nil)
	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d body = %s", recorder.Code, recorder.Body.String())
	}
	if !f.store.commandLookup || f.store.commandID != "CMD20260915000000deadbeef" {
		t.Fatalf("store lookup = %q (called=%v)", f.store.commandID, f.store.commandLookup)
	}
	data := payload["data"].(map[string]any)
	if data["commandId"] != "CMD20260915000000deadbeef" || data["result"] != "SUCCESS" || data["applied"] != true {
		t.Fatalf("device command = %#v", data)
	}
	if _, ok := data["commandNo"]; ok {
		t.Fatalf("response still carries the old name: %#v", data)
	}

	f.store.commandErr = ErrDeviceCommandNotFound
	recorder, payload = do(t, f.server.Handler(), http.MethodGet,
		"/api/v1/admin/device-commands/CMD20260915000000ffffffff", "", nil)
	if recorder.Code != http.StatusNotFound || payload["code"].(float64) != httpapi.CodeResourceNotFound {
		t.Fatalf("missing command: status = %d code = %v", recorder.Code, payload["code"])
	}

	// A user session must not reach an admin endpoint at all.
	user := newFixture(t, auth.Identity{ID: 7, Role: auth.RoleUser, Status: auth.StatusActive}, true)
	recorder, _ = do(t, user.server.Handler(), http.MethodGet, "/api/v1/admin/device-commands/CMD1", "", nil)
	if recorder.Code != http.StatusForbidden && recorder.Code != http.StatusUnauthorized {
		t.Fatalf("user status = %d, want 401/403", recorder.Code)
	}
}

// TestChangeStationStatus covers the station status endpoint: the write role
// reaches the store, an unregistered status is a 400 before any store call, an
// illegal transition surfaces as 409, an unknown station as 404, and the
// read-only AUDITOR role cannot write at all.
func TestChangeStationStatus(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	f.store.stationStatus = StationRecord{ID: 9, Code: "ST-9", Name: "站9", Status: station.StatusClosed}

	recorder, payload := do(t, f.server.Handler(), http.MethodPut, "/api/v1/admin/stations/9/status",
		`{"status":"CLOSED"}`, nil)
	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d body = %s", recorder.Code, recorder.Body.String())
	}
	if !f.store.stationStatusCalled || f.store.stationStatusCommand.StationID != 9 || f.store.stationStatusCommand.Status != station.StatusClosed {
		t.Fatalf("store command = %#v (called=%v)", f.store.stationStatusCommand, f.store.stationStatusCalled)
	}
	if payload["data"].(map[string]any)["status"] != station.StatusClosed {
		t.Fatalf("data = %#v", payload["data"])
	}

	// An unregistered status never reaches the store.
	f.store.stationStatusCalled = false
	recorder, payload = do(t, f.server.Handler(), http.MethodPut, "/api/v1/admin/stations/9/status",
		`{"status":"MAINTENANCE"}`, nil)
	if recorder.Code != http.StatusBadRequest || payload["code"].(float64) != httpapi.CodeInvalidArgument {
		t.Fatalf("unknown status: status = %d payload = %#v", recorder.Code, payload)
	}
	if f.store.stationStatusCalled {
		t.Fatal("an unregistered status reached the store")
	}

	recorder, _ = do(t, f.server.Handler(), http.MethodPut, "/api/v1/admin/stations/9/status", `{}`, nil)
	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("empty status = %d", recorder.Code)
	}
	recorder, _ = do(t, f.server.Handler(), http.MethodPut, "/api/v1/admin/stations/abc/status", `{"status":"CLOSED"}`, nil)
	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("bad id = %d", recorder.Code)
	}

	// The transition itself is decided by the store; the handler must not turn
	// the conflict into a success or a 500.
	f.store.stationStatusErr = ErrInvalidStateTransition
	recorder, _ = do(t, f.server.Handler(), http.MethodPut, "/api/v1/admin/stations/9/status", `{"status":"CLOSED"}`, nil)
	if recorder.Code != http.StatusConflict {
		t.Fatalf("illegal transition = %d body = %s", recorder.Code, recorder.Body.String())
	}
	f.store.stationStatusErr = ErrStationNotFound
	recorder, _ = do(t, f.server.Handler(), http.MethodPut, "/api/v1/admin/stations/9/status", `{"status":"CLOSED"}`, nil)
	if recorder.Code != http.StatusNotFound {
		t.Fatalf("missing station = %d", recorder.Code)
	}
	f.store.stationStatusErr = nil

	// AUDITOR is an administrator but read-only.
	auditor := newFixture(t, adminIdentity(auth.AdminRoleAuditor), true)
	recorder, _ = do(t, auditor.server.Handler(), http.MethodPut, "/api/v1/admin/stations/9/status", `{"status":"CLOSED"}`, nil)
	if recorder.Code != http.StatusForbidden {
		t.Fatalf("auditor write = %d, want 403", recorder.Code)
	}
}

// TestChangeChargerStatus is the charger half, including the in-use rejection
// that keeps a charger held by a live order out of the operator's reach.
func TestChangeChargerStatus(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleSuper), true)
	f.store.chargerStatus = ChargerStatusRecord{ChargerID: 5, ChargerCode: "C01", Status: station.ChargerStatusDisabled}

	recorder, payload := do(t, f.server.Handler(), http.MethodPut, "/api/v1/admin/chargers/5/status",
		`{"status":"DISABLED"}`, nil)
	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d body = %s", recorder.Code, recorder.Body.String())
	}
	if f.store.chargerStatusCommand.ChargerID != 5 || f.store.chargerStatusCommand.Status != station.ChargerStatusDisabled {
		t.Fatalf("store command = %#v", f.store.chargerStatusCommand)
	}
	if payload["data"].(map[string]any)["chargerId"].(float64) != 5 {
		t.Fatalf("data = %#v", payload["data"])
	}

	f.store.chargerStatusErr = ErrInvalidStateTransition
	recorder, _ = do(t, f.server.Handler(), http.MethodPut, "/api/v1/admin/chargers/5/status", `{"status":"IDLE"}`, nil)
	if recorder.Code != http.StatusConflict {
		t.Fatalf("illegal transition = %d", recorder.Code)
	}
	f.store.chargerStatusErr = nil

	recorder, _ = do(t, f.server.Handler(), http.MethodPut, "/api/v1/admin/chargers/5/status", `{"status":"BUSY"}`, nil)
	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("unknown status = %d", recorder.Code)
	}
	recorder, _ = do(t, f.server.Handler(), http.MethodGet, "/api/v1/admin/chargers/5/status", "", nil)
	if recorder.Code != http.StatusMethodNotAllowed {
		t.Fatalf("GET = %d, want 405", recorder.Code)
	}
}

// TestCreateStationResponseUsesContractKeys guards the fix that came out of the
// status-change work: StationRecord had no JSON tags, so POST /admin/stations
// answered with Go field names (ID, Code, Status) while the operation is
// registered as returning the Station schema, whose properties are camelCase.
func TestCreateStationResponseUsesContractKeys(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleSuper), true)
	f.store.createResult = StationRecord{ID: 3, Code: "ST-3", Name: "站3", Status: station.StatusOpen}

	recorder, payload := do(t, f.server.Handler(), http.MethodPost, "/api/v1/admin/stations",
		`{"code":"ST-3","name":"站3","address":"路1","latitudeE6":31230000,"longitudeE6":121470000}`,
		map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusCreated {
		t.Fatalf("status = %d body = %s", recorder.Code, recorder.Body.String())
	}
	data := payload["data"].(map[string]any)
	if data["id"].(float64) != 3 || data["code"] != "ST-3" || data["status"] != station.StatusOpen {
		t.Fatalf("data = %#v", data)
	}
	for _, wrong := range []string{"ID", "Code", "Status", "LatitudeE6"} {
		if _, ok := data[wrong]; ok {
			t.Fatalf("response still carries the Go-style key %q: %#v", wrong, data)
		}
	}
}
