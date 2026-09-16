package order

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net/http"
	"strings"
	"testing"
	"time"

	"github.com/heguangV/charging-station-platform/backend/internal/config"
	"github.com/heguangV/charging-station-platform/backend/internal/httpapi"
)

// The receipt endpoint is the charger gateway's way into the order service
// (BE-I-02). These tests cover what it accepts, what it refuses and what it
// hands to the service, because everything that reaches the service advances an
// order and starts or ends a bill.

const chargerEventToken = "gateway-service-token-01"

// receiptStore records the receipts that reach the order service and lets a test
// decide what the service answers.
type receiptStore struct {
	progress    []ConfirmProgressCommand
	progressErr error
	*fakeStore
	starts      []ConfirmStartCommand
	stops       []ConfirmStopCommand
	startResult Order
	stopResult  Order
	startErr    error
	stopErr     error
}

func (s *receiptStore) ConfirmStart(_ context.Context, command ConfirmStartCommand) (Order, error) {
	s.starts = append(s.starts, command)
	return s.startResult, s.startErr
}

func (s *receiptStore) ConfirmProgress(_ context.Context, command ConfirmProgressCommand) (Order, error) {
	s.progress = append(s.progress, command)
	if s.progressErr != nil {
		return Order{}, s.progressErr
	}
	return Order{OrderNo: command.OrderNo, Status: StatusCharging, MeteredEnergyWh: &command.EnergyWh}, nil
}

func (s *receiptStore) ConfirmStop(_ context.Context, command ConfirmStopCommand) (Order, error) {
	s.stops = append(s.stops, command)
	return s.stopResult, s.stopErr
}

func newReceiptFixture(t *testing.T, maxFutureSkew time.Duration) (http.Handler, *receiptStore) {
	t.Helper()
	store := &receiptStore{fakeStore: &fakeStore{}}
	service, err := NewService(store)
	if err != nil {
		t.Fatalf("NewService() error = %v", err)
	}
	handlers, err := NewChargerEventHandlers(service, ChargerEventConfig{
		Token:         chargerEventToken,
		MaxFutureSkew: maxFutureSkew,
		Logger:        slog.New(slog.NewTextHandler(io.Discard, nil)),
	})
	if err != nil {
		t.Fatalf("NewChargerEventHandlers() error = %v", err)
	}
	server := httpapi.NewServer(config.Config{RequestIDHeader: "X-Request-ID"}, slog.New(slog.NewTextHandler(io.Discard, nil)))
	handlers.Register(server)
	server.SetReady(true)
	return server.Handler(), store
}

func bearer() map[string]string {
	return map[string]string{"Authorization": "Bearer " + chargerEventToken}
}

func startedReceipt(occurredAt time.Time) string {
	return `{"eventId":"evt_receipt_0001","eventType":"CHARGE_STARTED","orderNo":"ORD20260914120000aaaa",` +
		`"chargerId":42,"occurredAt":"` + occurredAt.UTC().Format(time.RFC3339Nano) + `","traceId":"trace-gw"}`
}

func stoppedReceipt(occurredAt time.Time) string {
	return `{"eventId":"evt_receipt_0002","eventType":"CHARGE_STOPPED","orderNo":"ORD20260914120000aaaa",` +
		`"chargerId":42,"occurredAt":"` + occurredAt.UTC().Format(time.RFC3339Nano) + `",` +
		`"energyWh":1500,"meterStartWh":0,"meterEndWh":1500,"traceId":"trace-gw"}`
}

// A receipt without the service token is refused before anything else happens:
// a forged receipt would advance an order and start a bill.
func TestChargerEventEndpointRequiresTheServiceToken(t *testing.T) {
	handler, store := newReceiptFixture(t, 0)
	body := startedReceipt(time.Now().UTC())

	cases := map[string]map[string]string{
		"no header":            {},
		"another scheme":       {"Authorization": "Basic " + chargerEventToken},
		"wrong token":          {"Authorization": "Bearer not-the-token-0000"},
		"empty bearer":         {"Authorization": "Bearer "},
		"token in the url":     {"X-Service-Token": chargerEventToken},
		"token as the scheme":  {"Authorization": chargerEventToken},
		"lowercase scheme ok?": {"Authorization": "bearer " + chargerEventToken},
	}
	for name, headers := range cases {
		t.Run(name, func(t *testing.T) {
			recorder, _ := do(t, handler, http.MethodPost, ChargerEventPath, body, headers)
			// The scheme is case-insensitive per RFC 7235, so the last case is
			// the one accepted shape and must not be a 401.
			if name == "lowercase scheme ok?" {
				if recorder.Code != http.StatusOK {
					t.Fatalf("status = %d, want 200 for a case-insensitive scheme", recorder.Code)
				}
				return
			}
			if recorder.Code != http.StatusUnauthorized {
				t.Fatalf("status = %d, want 401", recorder.Code)
			}
		})
	}
	// No rejected request may reach the service.
	if len(store.starts)+len(store.stops) != 1 {
		t.Fatalf("only the accepted request may reach the service, got %d", len(store.starts)+len(store.stops))
	}
}

// An endpoint that cannot be authenticated must not start at all.
func TestNewChargerEventHandlersRefusesAnEmptyToken(t *testing.T) {
	store := &receiptStore{fakeStore: &fakeStore{}}
	service, err := NewService(store)
	if err != nil {
		t.Fatalf("NewService() error = %v", err)
	}
	if _, err := NewChargerEventHandlers(service, ChargerEventConfig{}); err == nil {
		t.Fatal("expected a missing service token to be refused")
	}
	if _, err := NewChargerEventHandlers(service, ChargerEventConfig{Token: "   "}); err == nil {
		t.Fatal("expected a blank service token to be refused")
	}
	if _, err := NewChargerEventHandlers(nil, ChargerEventConfig{Token: chargerEventToken}); err == nil {
		t.Fatal("expected a missing service to be refused")
	}
}

// A start receipt hands the device's fact to the service and answers with the
// order's new state, so the gateway can see that the fact was recorded.
func TestChargerEventEndpointAppliesAStartReceipt(t *testing.T) {
	handler, store := newReceiptFixture(t, 0)
	occurredAt := time.Now().UTC().Add(-30 * time.Second).Truncate(time.Millisecond)
	store.startResult = Order{OrderNo: "ORD20260914120000aaaa", Status: StatusCharging, ChargerID: 42, UserID: 7}

	recorder, payload := do(t, handler, http.MethodPost, ChargerEventPath, startedReceipt(occurredAt), bearer())
	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d, body = %s", recorder.Code, recorder.Body.String())
	}
	data, ok := payload["data"].(map[string]any)
	if !ok {
		t.Fatalf("unexpected envelope %v", payload)
	}
	if data["status"] != StatusCharging || data["eventId"] != "evt_receipt_0001" {
		t.Fatalf("unexpected response data %v", data)
	}

	if len(store.starts) != 1 || len(store.stops) != 0 {
		t.Fatalf("expected exactly one start confirmation, got %d/%d", len(store.starts), len(store.stops))
	}
	applied := store.starts[0]
	if applied.OrderNo != "ORD20260914120000aaaa" || applied.ChargerID != 42 {
		t.Fatalf("unexpected receipt %+v", applied)
	}
	if !applied.OccurredAt.Equal(occurredAt) {
		t.Fatalf("fact time = %s, want %s", applied.OccurredAt, occurredAt)
	}
	if applied.TraceID != "trace-gw" {
		t.Fatalf("trace id = %q, want the receipt's own trace id", applied.TraceID)
	}
	if len(applied.RequestHash) != 64 {
		t.Fatalf("payload digest = %q, want a sha256 hex digest", applied.RequestHash)
	}
}

// A stop receipt carries the metered energy that the bill is computed from.
func TestChargerEventEndpointAppliesAStopReceipt(t *testing.T) {
	handler, store := newReceiptFixture(t, 0)
	occurredAt := time.Now().UTC().Add(-time.Minute).Truncate(time.Millisecond)
	store.stopResult = Order{OrderNo: "ORD20260914120000aaaa", Status: StatusCompleted, ChargerID: 42, EnergyWh: 1500}

	recorder, payload := do(t, handler, http.MethodPost, ChargerEventPath, stoppedReceipt(occurredAt), bearer())
	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d, body = %s", recorder.Code, recorder.Body.String())
	}
	if len(store.stops) != 1 {
		t.Fatalf("expected one stop confirmation, got %d", len(store.stops))
	}
	applied := store.stops[0]
	if applied.EnergyWh != 1500 || applied.MeterStartWh == nil || *applied.MeterStartWh != 0 ||
		applied.MeterEndWh == nil || *applied.MeterEndWh != 1500 {
		t.Fatalf("unexpected stop receipt %+v", applied)
	}
	data, _ := payload["data"].(map[string]any)
	if data["status"] != StatusCompleted {
		t.Fatalf("unexpected response data %v", data)
	}
}

// A receipt the platform cannot act on is a 400 and never reaches the service.
func TestChargerEventEndpointRejectsMalformedReceipts(t *testing.T) {
	now := time.Now().UTC()
	tests := []struct {
		name   string
		body   string
		status int
	}{
		{"unknown event type", `{"eventId":"evt_receipt_0001","eventType":"CHARGE_PAUSED","orderNo":"ORD20260914120000aaaa","chargerId":42,"occurredAt":"` + now.Format(time.RFC3339) + `"}`, http.StatusBadRequest},
		{"missing event type", `{"eventId":"evt_receipt_0001","orderNo":"ORD20260914120000aaaa","chargerId":42,"occurredAt":"` + now.Format(time.RFC3339) + `"}`, http.StatusBadRequest},
		{"missing receipt id", `{"eventType":"CHARGE_STARTED","orderNo":"ORD20260914120000aaaa","chargerId":42,"occurredAt":"` + now.Format(time.RFC3339) + `"}`, http.StatusBadRequest},
		{"missing fact time", `{"eventId":"evt_receipt_0001","eventType":"CHARGE_STARTED","orderNo":"ORD20260914120000aaaa","chargerId":42}`, http.StatusBadRequest},
		{"missing charger", `{"eventId":"evt_receipt_0001","eventType":"CHARGE_STARTED","orderNo":"ORD20260914120000aaaa","occurredAt":"` + now.Format(time.RFC3339) + `"}`, http.StatusBadRequest},
		{"charger is not numeric", `{"eventId":"evt_receipt_0001","eventType":"CHARGE_STARTED","orderNo":"ORD20260914120000aaaa","chargerId":"C01","occurredAt":"` + now.Format(time.RFC3339) + `"}`, http.StatusBadRequest},
		{"stop without energy", `{"eventId":"evt_receipt_0002","eventType":"CHARGE_STOPPED","orderNo":"ORD20260914120000aaaa","chargerId":42,"occurredAt":"` + now.Format(time.RFC3339) + `"}`, http.StatusBadRequest},
		{"negative energy", `{"eventId":"evt_receipt_0002","eventType":"CHARGE_STOPPED","orderNo":"ORD20260914120000aaaa","chargerId":42,"energyWh":-1,"occurredAt":"` + now.Format(time.RFC3339) + `"}`, http.StatusBadRequest},
		{"unknown field", `{"eventId":"evt_receipt_0001","eventType":"CHARGE_STARTED","orderNo":"ORD20260914120000aaaa","chargerId":42,"occurredAt":"` + now.Format(time.RFC3339) + `","force":true}`, http.StatusBadRequest},
		{"second json value", `{"eventId":"evt_receipt_0001","eventType":"CHARGE_STARTED","orderNo":"ORD20260914120000aaaa","chargerId":42,"occurredAt":"` + now.Format(time.RFC3339) + `"} {}`, http.StatusBadRequest},
		{"not json", `not-json`, http.StatusBadRequest},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			handler, store := newReceiptFixture(t, 0)
			recorder, _ := do(t, handler, http.MethodPost, ChargerEventPath, test.body, bearer())
			if recorder.Code != test.status {
				t.Fatalf("status = %d, want %d (body %s)", recorder.Code, test.status, recorder.Body.String())
			}
			if len(store.starts)+len(store.stops) != 0 {
				t.Fatal("a malformed receipt must not reach the order service")
			}
		})
	}
}

// The fact time decides the billing window, so a non-UTC or future-dated
// occurredAt is refused rather than stored.
func TestChargerEventEndpointRejectsBadFactTimes(t *testing.T) {
	tests := []struct {
		name       string
		occurredAt string
	}{
		// A past instant carrying a non-zero offset: only the UTC rule can reject
		// this one, which is why the future-skew check cannot stand in for it.
		{"offset instead of utc", time.Now().UTC().Add(-time.Minute).In(time.FixedZone("CST", 8*3600)).Format(time.RFC3339)},
		{"far future", time.Now().UTC().Add(time.Hour).Format(time.RFC3339)},
		{"zero time", "0001-01-01T00:00:00Z"},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			handler, store := newReceiptFixture(t, 0)
			body := `{"eventId":"evt_receipt_0001","eventType":"CHARGE_STARTED","orderNo":"ORD20260914120000aaaa",` +
				`"chargerId":42,"occurredAt":"` + test.occurredAt + `"}`
			recorder, _ := do(t, handler, http.MethodPost, ChargerEventPath, body, bearer())
			if recorder.Code != http.StatusBadRequest {
				t.Fatalf("status = %d, want 400 (body %s)", recorder.Code, recorder.Body.String())
			}
			if len(store.starts) != 0 {
				t.Fatal("a rejected fact time must not reach the order service")
			}
		})
	}

	// The same receipt inside the allowed skew is accepted, so the rule rejects
	// outliers rather than any clock difference at all.
	handler, store := newReceiptFixture(t, 0)
	store.startResult = Order{OrderNo: "ORD20260914120000aaaa", Status: StatusCharging}
	withinSkew := time.Now().UTC().Add(30 * time.Second).Truncate(time.Millisecond)
	recorder, _ := do(t, handler, http.MethodPost, ChargerEventPath, startedReceipt(withinSkew), bearer())
	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200 for a fact time inside the skew", recorder.Code)
	}
	if len(store.starts) != 1 {
		t.Fatal("expected the receipt inside the skew to be applied")
	}
}

// The configured skew is honoured rather than a hard-coded default.
func TestChargerEventEndpointHonoursTheConfiguredSkew(t *testing.T) {
	handler, store := newReceiptFixture(t, time.Hour)
	store.startResult = Order{OrderNo: "ORD20260914120000aaaa", Status: StatusCharging}
	occurredAt := time.Now().UTC().Add(30 * time.Minute).Truncate(time.Millisecond)

	recorder, _ := do(t, handler, http.MethodPost, ChargerEventPath, startedReceipt(occurredAt), bearer())
	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200 with a one-hour skew (body %s)", recorder.Code, recorder.Body.String())
	}
	if len(store.starts) != 1 || !store.starts[0].OccurredAt.Equal(occurredAt) {
		t.Fatalf("unexpected receipts %+v", store.starts)
	}

	service, err := NewService(&receiptStore{fakeStore: &fakeStore{}})
	if err != nil {
		t.Fatalf("NewService() error = %v", err)
	}
	if err := service.SetFactTimeSkew(0); err == nil {
		t.Fatal("expected a non-positive skew to be refused")
	}
}

// The digest is what makes a retry safe: the same fact reformatted must replay,
// a different fact must conflict.
func TestChargerEventDigestCoversTheFactNotTheBytes(t *testing.T) {
	occurredAt := time.Now().UTC().Truncate(time.Second)
	first := chargerEventRequest{
		EventID: "evt_receipt_0001", EventType: chargerEventStarted, OrderNo: "ORD20260914120000aaaa",
		ChargerID: chargerIDField{value: 42}, OccurredAt: occurredAt,
	}
	// The same receipt as the gateway might resend it: charger id as a string,
	// a different offset for the same instant, fields in another order.
	resend := chargerEventRequest{
		EventID: " evt_receipt_0001 ", EventType: chargerEventStarted, OrderNo: "ORD20260914120000aaaa",
		ChargerID: chargerIDField{value: 42}, OccurredAt: occurredAt.In(time.FixedZone("UTC+0", 0)),
	}
	if chargerEventDigest(chargerEventStarted, first) != chargerEventDigest(chargerEventStarted, resend) {
		t.Fatal("the same fact must digest identically, otherwise a retry conflicts with itself")
	}

	different := first
	different.OrderNo = "ORD20260914120000bbbb"
	if chargerEventDigest(chargerEventStarted, first) == chargerEventDigest(chargerEventStarted, different) {
		t.Fatal("a different order must digest differently")
	}
	differentEnergy := chargerEventRequest{
		EventID: "evt_receipt_0002", EventType: chargerEventStopped, OrderNo: "ORD20260914120000aaaa",
		ChargerID: chargerIDField{value: 42}, OccurredAt: occurredAt,
	}
	withEnergy := differentEnergy
	energy := int64(1500)
	withEnergy.EnergyWh = &energy
	if chargerEventDigest(chargerEventStopped, differentEnergy) == chargerEventDigest(chargerEventStopped, withEnergy) {
		t.Fatal("a stop with metered energy is a different fact than one without")
	}
	// And the same stop is stable.
	if chargerEventDigest(chargerEventStopped, withEnergy) != chargerEventDigest(chargerEventStopped, withEnergy) {
		t.Fatal("the digest must be deterministic")
	}
}

// A receipt that contradicts the order is a 409 and not a 400: the payload is
// well formed, the fact is impossible.
func TestChargerEventEndpointMapsDomainRejections(t *testing.T) {
	tests := []struct {
		name     string
		startErr error
		stopErr  error
		status   int
	}{
		{"order not found", ErrOrderNotFound, nil, http.StatusNotFound},
		{"stop before start", nil, ErrFactTimeOutOfOrder, http.StatusConflict},
		{"receipt for another charger", ErrChargerOrderMismatch, nil, http.StatusConflict},
		{"order already past this fact", ErrInvalidStateTransition, nil, http.StatusConflict},
		{"database unavailable", errors.New("connection refused"), nil, http.StatusServiceUnavailable},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			handler, store := newReceiptFixture(t, 0)
			store.startErr = test.startErr
			store.stopErr = test.stopErr
			body := startedReceipt(time.Now().UTC().Add(-time.Second))
			if test.stopErr != nil {
				body = stoppedReceipt(time.Now().UTC().Add(-time.Second))
			}
			recorder, payload := do(t, handler, http.MethodPost, ChargerEventPath, body, bearer())
			if recorder.Code != test.status {
				t.Fatalf("status = %d, want %d (body %s)", recorder.Code, test.status, recorder.Body.String())
			}
			if payload["success"] != false {
				t.Fatalf("a rejected receipt must answer with a failure envelope, got %v", payload)
			}
		})
	}
}

// Only POST is accepted on the internal route.
func TestChargerEventEndpointRejectsOtherMethods(t *testing.T) {
	handler, _ := newReceiptFixture(t, 0)
	recorder, _ := do(t, handler, http.MethodGet, ChargerEventPath, "", bearer())
	if recorder.Code != http.StatusMethodNotAllowed {
		t.Fatalf("status = %d, want 405", recorder.Code)
	}
}

// A receipt without its own trace id still has to be traceable: the request id
// stands in for it.
func TestChargerEventEndpointFallsBackToTheRequestID(t *testing.T) {
	handler, store := newReceiptFixture(t, 0)
	store.startResult = Order{OrderNo: "ORD20260914120000aaaa", Status: StatusCharging}
	body := `{"eventId":"evt_receipt_0001","eventType":"CHARGE_STARTED","orderNo":"ORD20260914120000aaaa",` +
		`"chargerId":"42","occurredAt":"` + time.Now().UTC().Add(-time.Second).Format(time.RFC3339) + `"}`
	headers := bearer()
	headers["X-Request-ID"] = "req-abcdef123456"

	recorder, _ := do(t, handler, http.MethodPost, ChargerEventPath, body, headers)
	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d, body = %s", recorder.Code, recorder.Body.String())
	}
	if len(store.starts) != 1 || store.starts[0].TraceID != "req-abcdef123456" {
		t.Fatalf("expected the request id as the trace id, got %+v", store.starts)
	}
	if store.starts[0].ChargerID != 42 {
		t.Fatalf("a numeric charger id sent as a string must be accepted, got %d", store.starts[0].ChargerID)
	}
}

// The success envelope is what the gateway records, so its shape is part of the
// contract: the applied receipt id and the order's new state.
func TestChargerEventResponseShape(t *testing.T) {
	handler, store := newReceiptFixture(t, 0)
	store.stopResult = Order{OrderNo: "ORD20260914120000aaaa", Status: StatusCompleted, EnergyWh: 1500, AmountCent: 180}

	recorder, _ := do(t, handler, http.MethodPost, ChargerEventPath, stoppedReceipt(time.Now().UTC().Add(-time.Minute)), bearer())
	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d", recorder.Code)
	}
	var decoded struct {
		Success bool   `json:"success"`
		Code    int    `json:"code"`
		Message string `json:"message"`
		Data    struct {
			EventID   string `json:"eventId"`
			EventType string `json:"eventType"`
			OrderNo   string `json:"orderNo"`
			Status    string `json:"status"`
			Order     Order  `json:"order"`
		} `json:"data"`
	}
	if err := json.Unmarshal(recorder.Body.Bytes(), &decoded); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if !decoded.Success || decoded.Code != httpapi.CodeOK || decoded.Message != "ok" {
		t.Fatalf("unexpected envelope %+v", decoded)
	}
	if decoded.Data.EventID != "evt_receipt_0002" || decoded.Data.EventType != chargerEventStopped ||
		decoded.Data.OrderNo != "ORD20260914120000aaaa" || decoded.Data.Status != StatusCompleted {
		t.Fatalf("unexpected data %+v", decoded.Data)
	}
	if decoded.Data.Order.AmountCent != 180 || decoded.Data.Order.EnergyWh != 1500 {
		t.Fatalf("the response must carry the order the receipt produced, got %+v", decoded.Data.Order)
	}
	// The route is registered on the API server's own mux, so an unregistered
	// path is what it should be: not this endpoint.
	recorder, _ = do(t, handler, http.MethodPost, ChargerEventPath+"/extra", stoppedReceipt(time.Now().UTC()), bearer())
	if recorder.Code != http.StatusNotFound {
		t.Fatalf("status = %d, want 404 for an unknown path", recorder.Code)
	}
}

// The receipt path is internal: the user-facing order routes must not be
// reachable through it, and it must not be reachable through theirs.
func TestChargerEventRouteIsSeparateFromTheUserRoutes(t *testing.T) {
	handler, _ := newReceiptFixture(t, 0)
	recorder, _ := do(t, handler, http.MethodPost, "/api/v1/orders/ORD20260914120000aaaa/start", `{}`, bearer())
	if recorder.Code != http.StatusNotFound {
		t.Fatalf("status = %d, want 404: the receipt endpoint registers only its own route", recorder.Code)
	}
	if !strings.HasPrefix(ChargerEventPath, "/api/v1/internal/") {
		t.Fatalf("the internal route must stay under /api/v1/internal, got %s", ChargerEventPath)
	}
}
