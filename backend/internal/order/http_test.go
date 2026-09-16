package order

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
	"time"

	"github.com/heguangV/charging-station-platform/backend/internal/auth"
	"github.com/heguangV/charging-station-platform/backend/internal/config"
	"github.com/heguangV/charging-station-platform/backend/internal/httpapi"
)

// fakeStore records commands and returns canned results.
type fakeStore struct {
	createResult   Order
	createErr      error
	startResult    Order
	startErr       error
	stopResult     Order
	stopErr        error
	getResult      Order
	getErr         error
	listResult     OrderPage
	listErr        error
	listFilter     ListFilter
	listFilterSet  bool
	createCommands []CreateOrderCommand
	startCommands  []TransitionCommand
	stopCommands   []TransitionCommand
}

func (f *fakeStore) CreateOrder(_ context.Context, command CreateOrderCommand) (Order, error) {
	f.createCommands = append(f.createCommands, command)
	return f.createResult, f.createErr
}

func (f *fakeStore) StartCharging(_ context.Context, command TransitionCommand) (Order, error) {
	f.startCommands = append(f.startCommands, command)
	return f.startResult, f.startErr
}

func (f *fakeStore) StopCharging(_ context.Context, command TransitionCommand) (Order, error) {
	f.stopCommands = append(f.stopCommands, command)
	return f.stopResult, f.stopErr
}

func (f *fakeStore) CancelOrder(_ context.Context, command TransitionCommand) (Order, error) {
	f.stopCommands = append(f.stopCommands, command)
	return f.stopResult, f.stopErr
}

func (f *fakeStore) ExpireStaleOrders(context.Context, time.Duration) (int, error) {
	return 0, nil
}

func (f *fakeStore) SettleOrder(_ context.Context, command SettleCommand) (Order, error) {
	return f.stopResult, f.stopErr
}

func (f *fakeStore) ReleaseOrphanedChargers(context.Context) (int, error) {
	return 0, nil
}

func (f *fakeStore) CompleteChargerCommand(context.Context, int64, string, string) (bool, error) {
	return false, nil
}

func (f *fakeStore) RecordChargerCommandResult(context.Context, string, string, int64, string, string, string) (bool, error) {
	return false, nil
}

func (f *fakeStore) ReissueStopCommands(context.Context, StopRecoveryPolicy) (StopRecoveryResult, error) {
	return StopRecoveryResult{}, nil
}

func (f *fakeStore) ConfirmStart(context.Context, ConfirmStartCommand) (Order, error) {
	return Order{}, nil
}

func (f *fakeStore) ConfirmStop(context.Context, ConfirmStopCommand) (Order, error) {
	return Order{}, nil
}

func (f *fakeStore) GetOrderByNo(context.Context, int64, string) (Order, error) {
	return f.getResult, f.getErr
}

func (f *fakeStore) ListOrdersByUser(_ context.Context, filter ListFilter) (OrderPage, error) {
	f.listFilter = filter
	f.listFilterSet = true
	return f.listResult, f.listErr
}

// fakeAuthProvider mimics auth.Handlers middleware for handler tests.
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

const userSession = `{"id":7,"role":"USER","displayName":"开发用户","status":"ACTIVE"}`

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

const idemKey = "idempotency-key-000001"
const validCreate = `{"chargerId":1}`

func TestCreateOrderSuccess(t *testing.T) {
	f := newFixture(t, auth.Identity{ID: 7, Role: auth.RoleUser, Status: auth.StatusActive}, true)
	f.store.createResult = Order{
		OrderNo: "ORD20260914120000aaaa", UserID: 7, StationID: 1, ChargerID: 1,
		Status: StatusCreated, CreatedAt: time.Now().UTC(),
	}

	recorder, payload := do(t, f.server.Handler(), http.MethodPost, "/api/v1/orders", validCreate,
		map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusCreated {
		t.Fatalf("status = %d body = %s", recorder.Code, recorder.Body.String())
	}
	data := payload["data"].(map[string]any)
	if data["orderNo"] != "ORD20260914120000aaaa" || data["status"] != StatusCreated || data["userId"].(float64) != 7 {
		t.Fatalf("order = %#v", data)
	}

	command := f.store.createCommands[0]
	if command.UserID != 7 || command.ChargerID != 1 || command.IdempotencyKey != idemKey || command.RequestHash == "" {
		t.Fatalf("command = %#v", command)
	}
}

func TestCreateOrderValidationAndBusinessErrors(t *testing.T) {
	f := newFixture(t, auth.Identity{ID: 7, Role: auth.RoleUser, Status: auth.StatusActive}, true)

	// malformed body / missing key / bad charger id
	recorder, payload := do(t, f.server.Handler(), http.MethodPost, "/api/v1/orders", `{bad`, map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusBadRequest || payload["code"].(float64) != httpapi.CodeInvalidArgument {
		t.Fatalf("bad body: status = %d code = %v", recorder.Code, payload["code"])
	}
	recorder, _ = do(t, f.server.Handler(), http.MethodPost, "/api/v1/orders", validCreate, nil)
	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("missing key status = %d", recorder.Code)
	}
	recorder, _ = do(t, f.server.Handler(), http.MethodPost, "/api/v1/orders", `{"chargerId":0}`, map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("charger 0 status = %d", recorder.Code)
	}

	// business errors land on their registry codes
	business := []struct {
		name string
		err  error
		code float64
	}{
		{"insufficient balance", ErrInsufficientBalance, codeInsufficientBalance},
		{"charger unavailable", ErrChargerUnavailable, codeChargerUnavailable},
		{"active flow exists", ErrActiveFlowExists, codeActiveFlowExists},
		{"idempotency conflict", ErrIdempotencyConflict, codeIdempotencyConflict},
		{"in progress", ErrIdempotencyInProgress, codeIdempotencyConflict},
	}
	for _, testCase := range business {
		f.store.createErr = testCase.err
		recorder, payload := do(t, f.server.Handler(), http.MethodPost, "/api/v1/orders", validCreate, map[string]string{"Idempotency-Key": idemKey})
		if recorder.Code != http.StatusConflict || payload["code"].(float64) != testCase.code {
			t.Errorf("%s: status = %d code = %v, want 409/%v", testCase.name, recorder.Code, payload["code"], testCase.code)
		}
	}
}

func TestTransitionEndpointsAndReplayShape(t *testing.T) {
	f := newFixture(t, auth.Identity{ID: 7, Role: auth.RoleUser, Status: auth.StatusActive}, true)
	f.store.startResult = Order{OrderNo: "ORD20260914120000aaaa", UserID: 7, Status: StatusStarting}

	recorder, payload := do(t, f.server.Handler(), http.MethodPost, "/api/v1/orders/ORD20260914120000aaaa/start", "",
		map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusAccepted {
		t.Fatalf("start status = %d body = %s", recorder.Code, recorder.Body.String())
	}
	if payload["data"].(map[string]any)["status"] != StatusStarting {
		t.Fatalf("start data = %#v", payload["data"])
	}
	command := f.store.startCommands[0]
	if command.OrderNo != "ORD20260914120000aaaa" || command.IdempotencyKey != idemKey {
		t.Fatalf("start command = %#v", command)
	}

	f.store.stopResult = Order{OrderNo: "ORD20260914120000aaaa", Status: StatusStopping}
	recorder, _ = do(t, f.server.Handler(), http.MethodPost, "/api/v1/orders/ORD20260914120000aaaa/stop", "",
		map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusAccepted {
		t.Fatalf("stop status = %d", recorder.Code)
	}

	// missing key on start
	recorder, _ = do(t, f.server.Handler(), http.MethodPost, "/api/v1/orders/ORD20260914120000aaaa/start", "", nil)
	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("start without key status = %d", recorder.Code)
	}

	// wrong state
	f.store.startErr = ErrInvalidStateTransition
	recorder, payload = do(t, f.server.Handler(), http.MethodPost, "/api/v1/orders/ORD20260914120000aaaa/start", "",
		map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusConflict || payload["code"].(float64) != codeInvalidStateTransition {
		t.Fatalf("invalid transition: status = %d code = %v", recorder.Code, payload["code"])
	}
}

func TestListAndGetEndpoints(t *testing.T) {
	f := newFixture(t, auth.Identity{ID: 7, Role: auth.RoleUser, Status: auth.StatusActive}, true)
	f.store.listResult = OrderPage{
		Items: []Order{{OrderNo: "ORD20260914120000aaaa", UserID: 7, Status: StatusCreated}},
		Meta:  PageMeta{Page: 1, PageSize: 20, Total: 1},
	}

	recorder, payload := do(t, f.server.Handler(), http.MethodGet, "/api/v1/orders?status=CREATED", "", nil)
	if recorder.Code != http.StatusOK {
		t.Fatalf("list status = %d", recorder.Code)
	}
	data := payload["data"].(map[string]any)
	if len(data["items"].([]any)) != 1 || data["meta"].(map[string]any)["total"].(float64) != 1 {
		t.Fatalf("list data = %#v", data)
	}

	recorder, payload = do(t, f.server.Handler(), http.MethodGet, "/api/v1/orders?status=NOPE", "", nil)
	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("bad filter status = %d", recorder.Code)
	}

	f.store.getResult = Order{OrderNo: "ORD20260914120000aaaa", UserID: 7}
	recorder, payload = do(t, f.server.Handler(), http.MethodGet, "/api/v1/orders/ORD20260914120000aaaa", "", nil)
	if recorder.Code != http.StatusOK || payload["data"].(map[string]any)["orderNo"] == "" {
		t.Fatalf("get: status = %d payload = %#v", recorder.Code, payload)
	}

	f.store.getErr = ErrOrderNotFound
	recorder, payload = do(t, f.server.Handler(), http.MethodGet, "/api/v1/orders/ORD20260914120000aaaa", "", nil)
	if recorder.Code != http.StatusNotFound || payload["code"].(float64) != httpapi.CodeResourceNotFound {
		t.Fatalf("get missing: status = %d code = %v", recorder.Code, payload["code"])
	}

	recorder, _ = do(t, f.server.Handler(), http.MethodGet, "/api/v1/orders/short", "", nil)
	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("short orderNo status = %d", recorder.Code)
	}
}

func TestAdminsAndAnonymousCannotUseUserOrderRoutes(t *testing.T) {
	admin := newFixture(t, auth.Identity{ID: 1, Role: auth.RoleAdmin, Status: auth.StatusActive}, true)
	recorder, payload := do(t, admin.server.Handler(), http.MethodPost, "/api/v1/orders", validCreate, map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusForbidden || payload["code"].(float64) != httpapi.CodeForbidden {
		t.Fatalf("admin create: status = %d code = %v", recorder.Code, payload["code"])
	}

	anonymous := newFixture(t, auth.Identity{}, false)
	recorder, _ = do(t, anonymous.server.Handler(), http.MethodGet, "/api/v1/orders", "", nil)
	if recorder.Code != http.StatusUnauthorized {
		t.Fatalf("anonymous list status = %d", recorder.Code)
	}
}

func TestWrongMethodOnOrderRoutes(t *testing.T) {
	f := newFixture(t, auth.Identity{ID: 7, Role: auth.RoleUser, Status: auth.StatusActive}, true)
	recorder, _ := do(t, f.server.Handler(), http.MethodDelete, "/api/v1/orders", "", nil)
	if recorder.Code != http.StatusMethodNotAllowed {
		t.Fatalf("delete orders status = %d", recorder.Code)
	}
	recorder, _ = do(t, f.server.Handler(), http.MethodDelete, "/api/v1/orders/ORD20260914120000aaaa", "", nil)
	if recorder.Code != http.StatusMethodNotAllowed {
		t.Fatalf("delete order status = %d", recorder.Code)
	}
}

func TestDefaultErrorIsNotACredentialLeak(t *testing.T) {
	f := newFixture(t, auth.Identity{ID: 7, Role: auth.RoleUser, Status: auth.StatusActive}, true)
	f.store.createErr = errors.New("connection reset by peer")

	recorder, payload := do(t, f.server.Handler(), http.MethodPost, "/api/v1/orders", validCreate, map[string]string{"Idempotency-Key": idemKey})
	if recorder.Code != http.StatusServiceUnavailable || payload["code"].(float64) != httpapi.CodeDatabaseError {
		t.Fatalf("default error: status = %d code = %v", recorder.Code, payload["code"])
	}
	if strings.Contains(recorder.Body.String(), "connection reset") {
		t.Fatal("internal error text leaked into the response")
	}
}

func TestConfirmStopValidatesMeterReadings(t *testing.T) {
	f := newFixture(t, auth.Identity{ID: 7, Role: auth.RoleUser, Status: auth.StatusActive}, true)
	service, err := NewService(f.store)
	if err != nil {
		t.Fatalf("NewService() error = %v", err)
	}
	ctx := context.Background()
	start := int64(100)
	end := int64(99)

	if _, err := service.ConfirmStop(ctx, ConfirmStopCommand{
		OrderNo: "ORD20260914120000aaaa", ChargerID: 7, EventID: "evt_meter_0001", OccurredAt: time.Now().UTC(),
		EnergyWh: 100, MeterStartWh: &start, MeterEndWh: &end,
	}); err == nil {
		t.Fatal("end reading below start reading accepted")
	}

	end = int64(250)
	if _, err := service.ConfirmStop(ctx, ConfirmStopCommand{
		OrderNo: "ORD20260914120000aaaa", ChargerID: 7, EventID: "evt_meter_0001", OccurredAt: time.Now().UTC(),
		EnergyWh: 100, MeterStartWh: &start, MeterEndWh: &end,
	}); err == nil {
		t.Fatal("energy inconsistent with meter readings accepted")
	}

	// Consistent readings pass validation and reach the store.
	end = int64(200)
	if _, err := service.ConfirmStop(ctx, ConfirmStopCommand{
		OrderNo: "ORD20260914120000aaaa", ChargerID: 7, EventID: "evt_meter_0001", OccurredAt: time.Now().UTC(),
		EnergyWh: 100, MeterStartWh: &start, MeterEndWh: &end,
	}); err != nil {
		t.Fatalf("consistent readings rejected: %v", err)
	}
}

// TestListAcceptsTheRegisteredQuerySurface covers the three query parameters the
// contract registers for the order list and the front end sends: createdFrom,
// createdTo and sort. The point of the test is that they reach the service (a
// silently ignored filter looks identical to a working one from the outside) and
// that the two ways of asking for nothing - an unparseable date and a window
// whose end is before its start - are rejected instead of answered with the full
// list.
func TestListAcceptsTheRegisteredQuerySurface(t *testing.T) {
	f := newFixture(t, auth.Identity{ID: 7, Role: auth.RoleUser, Status: auth.StatusActive}, true)
	f.store.listResult = OrderPage{Items: []Order{}, Meta: PageMeta{Page: 1, PageSize: 20}}

	recorder, _ := do(t, f.server.Handler(), http.MethodGet,
		"/api/v1/orders?createdFrom=2026-01-01T00:00:00Z&createdTo=2026-02-01T00:00:00Z&sort=createdAt", "", nil)
	if recorder.Code != http.StatusOK {
		t.Fatalf("registered filter status = %d body = %s", recorder.Code, recorder.Body.String())
	}
	if !f.store.listFilterSet {
		t.Fatal("the filter never reached the service")
	}
	if f.store.listFilter.Sort != SortCreatedAtAsc {
		t.Fatalf("sort = %q, want %q", f.store.listFilter.Sort, SortCreatedAtAsc)
	}
	wantFrom := time.Date(2026, 1, 1, 0, 0, 0, 0, time.UTC)
	if f.store.listFilter.CreatedFrom == nil || !f.store.listFilter.CreatedFrom.Equal(wantFrom) {
		t.Fatalf("createdFrom = %v, want %v", f.store.listFilter.CreatedFrom, wantFrom)
	}
	if f.store.listFilter.CreatedTo == nil || !f.store.listFilter.CreatedTo.Equal(time.Date(2026, 2, 1, 0, 0, 0, 0, time.UTC)) {
		t.Fatalf("createdTo = %v", f.store.listFilter.CreatedTo)
	}

	for _, tc := range []struct{ name, query string }{
		{"unparseable createdFrom", "createdFrom=2026-01-01"},
		{"window end before start", "createdFrom=2026-02-01T00:00:00Z&createdTo=2026-01-01T00:00:00Z"},
		{"unregistered sort", "sort=created_at"},
	} {
		recorder, payload := do(t, f.server.Handler(), http.MethodGet, "/api/v1/orders?"+tc.query, "", nil)
		if recorder.Code != http.StatusBadRequest {
			t.Fatalf("%s status = %d body = %s", tc.name, recorder.Code, recorder.Body.String())
		}
		if payload["code"].(float64) != httpapi.CodeInvalidArgument {
			t.Fatalf("%s code = %v", tc.name, payload["code"])
		}
	}
}
