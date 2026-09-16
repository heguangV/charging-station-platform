package wallet

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/heguangV/charging-station-platform/backend/internal/auth"
	"github.com/heguangV/charging-station-platform/backend/internal/httpapi"
)

// This file covers the HTTP mapping of the refund endpoint. The mapping is the
// only place where a missing order and an order with nothing to refund become
// distinguishable to a caller, so it is asserted end to end through the handler
// rather than through writeWalletError alone.

// adminIdentity lets a request through with the given admin identity, standing
// in for auth's RequireAdminWrite middleware.
type adminIdentity struct{ id int64 }

func (a adminIdentity) RequireRole(role string, next http.HandlerFunc) http.HandlerFunc {
	if role == auth.RoleAdmin {
		return a.with(next)
	}
	return func(w http.ResponseWriter, r *http.Request) {
		httpapi.WriteError(w, r, http.StatusForbidden, httpapi.CodeForbidden, "insufficient permission", nil)
	}
}

func (a adminIdentity) RequireAdminWrite(next http.HandlerFunc) http.HandlerFunc {
	return a.with(next)
}

func (a adminIdentity) with(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		next(w, r.WithContext(auth.WithIdentity(r.Context(), auth.Identity{ID: a.id, Role: "ADMIN", AdminRole: "OPERATOR"})))
	}
}

type routeRegistry map[string]http.HandlerFunc

func (r routeRegistry) Register(pattern string, handler http.HandlerFunc) {
	r[pattern] = handler
}

func TestWalletRoutesRejectAdminIdentity(t *testing.T) {
	service, err := NewService(&fakeStore{})
	if err != nil {
		t.Fatalf("NewService() error = %v", err)
	}
	handlers, err := NewHandlers(service, adminIdentity{id: 7})
	if err != nil {
		t.Fatalf("NewHandlers() error = %v", err)
	}
	routes := routeRegistry{}
	handlers.Register(routes)

	for _, path := range []string{"/api/v1/wallet", "/api/v1/wallet/top-up", "/api/v1/wallet/transactions"} {
		recorder := httptest.NewRecorder()
		routes[path](recorder, httptest.NewRequest(http.MethodGet, path, nil))
		if recorder.Code != http.StatusForbidden {
			t.Errorf("%s status = %d, want 403", path, recorder.Code)
		}
	}
}

// refundRequest builds a refund request that reaches the service: an admin
// session, an order number in the path and a valid Idempotency-Key header.
func refundRequest(t *testing.T, handlers *Handlers, orderNo string) *httptest.ResponseRecorder {
	t.Helper()
	request := httptest.NewRequest(http.MethodPost, "/api/v1/admin/orders/"+orderNo+"/refund", strings.NewReader(`{"reason":"integration test"}`))
	request.Header.Set("Idempotency-Key", "123e4567-e89b-12d3-a456-426614174002")
	request.SetPathValue("orderNo", orderNo)
	recorder := httptest.NewRecorder()
	// The middleware is applied here because the handler is called directly
	// rather than through Register.
	adminIdentity{id: 9}.RequireAdminWrite(handlers.refundOrder)(recorder, request)
	return recorder
}

// TestRefundMissingOrderIs404 keeps the refund contract's two failure modes
// apart: an order that does not exist is 404/NOT_FOUND, while an order with
// nothing to refund stays 409/18. Collapsing them into one status leaves the
// caller unable to tell a typo in the order number from an order that was
// already refunded.
func TestRefundMissingOrderIs404(t *testing.T) {
	service, err := NewService(&fakeStore{refundErr: ErrOrderNotFound})
	if err != nil {
		t.Fatalf("NewService() error = %v", err)
	}
	handlers, err := NewHandlers(service, adminIdentity{id: 9})
	if err != nil {
		t.Fatalf("NewHandlers() error = %v", err)
	}

	recorder := refundRequest(t, handlers, "ORD-MISSING-0001")
	if recorder.Code != http.StatusNotFound {
		t.Fatalf("status = %d, want 404 (body %s)", recorder.Code, recorder.Body.String())
	}
	var body struct {
		Success bool   `json:"success"`
		Code    int    `json:"code"`
		Message string `json:"message"`
	}
	if err := json.Unmarshal(recorder.Body.Bytes(), &body); err != nil {
		t.Fatalf("decode response %q: %v", recorder.Body.String(), err)
	}
	if body.Success || body.Code != 4 {
		t.Fatalf("response = %#v, want success=false and code 4", body)
	}
}

// TestRefundNotRefundableStays409 is the companion assertion: the two modes are
// not merely different from 200, they are different from each other.
func TestRefundNotRefundableStays409(t *testing.T) {
	service, err := NewService(&fakeStore{refundErr: ErrOrderNotRefundable})
	if err != nil {
		t.Fatalf("NewService() error = %v", err)
	}
	handlers, err := NewHandlers(service, adminIdentity{id: 9})
	if err != nil {
		t.Fatalf("NewHandlers() error = %v", err)
	}

	recorder := refundRequest(t, handlers, "ORD-ALREADY-REFUNDED-0001")
	if recorder.Code != http.StatusConflict {
		t.Fatalf("status = %d, want 409 (body %s)", recorder.Code, recorder.Body.String())
	}
	var body struct {
		Code int `json:"code"`
	}
	if err := json.Unmarshal(recorder.Body.Bytes(), &body); err != nil {
		t.Fatalf("decode response %q: %v", recorder.Body.String(), err)
	}
	if body.Code != 18 {
		t.Fatalf("code = %d, want 18", body.Code)
	}
}

func TestRefundRejectsNonStrictJSON(t *testing.T) {
	service, err := NewService(&fakeStore{})
	if err != nil {
		t.Fatalf("NewService() error = %v", err)
	}
	handlers, err := NewHandlers(service, adminIdentity{id: 9})
	if err != nil {
		t.Fatalf("NewHandlers() error = %v", err)
	}

	for _, body := range []string{
		`{"reason":"integration test","amountCent":1}`,
		`{"reason":"integration test"} {}`,
	} {
		request := httptest.NewRequest(http.MethodPost, "/api/v1/admin/orders/ORD-REFUND-0001/refund", strings.NewReader(body))
		request.Header.Set("Idempotency-Key", "123e4567-e89b-12d3-a456-426614174002")
		request.SetPathValue("orderNo", "ORD-REFUND-0001")
		recorder := httptest.NewRecorder()
		adminIdentity{id: 9}.RequireAdminWrite(handlers.refundOrder)(recorder, request)

		var response struct {
			Code int `json:"code"`
		}
		if err := json.Unmarshal(recorder.Body.Bytes(), &response); err != nil {
			t.Fatalf("decode response %q: %v", recorder.Body.String(), err)
		}
		if recorder.Code != http.StatusBadRequest || response.Code != httpapi.CodeInvalidArgument {
			t.Errorf("body %q: status = %d code = %d, want 400/%d", body, recorder.Code, response.Code, httpapi.CodeInvalidArgument)
		}
	}
}
