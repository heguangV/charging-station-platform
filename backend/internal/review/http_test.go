package review

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

type fakeStore struct {
	review        ReviewView
	reviewErr     error
	appeal        AppealView
	appealErr     error
	wall          WallPage
	approved      bool
	approveResult bool
	approveErr    error
	rejected      bool
	rejectReason  string
	rejectResult  bool
	rejectErr     error
}

func (f *fakeStore) CreateReview(_ context.Context, userID int64, orderNo string, stars int, comment string) (ReviewView, error) {
	return ReviewView{OrderNo: orderNo, Stars: stars, Comment: comment, CreatedAt: time.Now().UTC()}, f.reviewErr
}

func (f *fakeStore) GetReview(context.Context, int64, string) (ReviewView, error) {
	return f.review, f.reviewErr
}

func (f *fakeStore) ListWall(context.Context, WallFilter) (WallPage, error) {
	return f.wall, nil
}

func (f *fakeStore) CreateAppeal(_ context.Context, userID int64, orderNo string, reason string) (AppealView, error) {
	return AppealView{ID: 1, OrderNo: orderNo, Reason: reason, Status: AppealPending}, f.appealErr
}

func (f *fakeStore) ListAppeals(context.Context, AppealFilter) (AppealPage, error) {
	return AppealPage{Items: []AppealView{{ID: 1, Status: AppealPending}}, Meta: PageMeta{Total: 1}}, nil
}

func (f *fakeStore) GetAppeal(_ context.Context, appealID int64) (AppealView, error) {
	status := AppealPending
	if f.approved {
		status = AppealApproved
	}
	return AppealView{ID: appealID, Status: status}, nil
}

func (f *fakeStore) ApproveAppeal(context.Context, int64, int64) (bool, error) {
	f.approved = true
	return f.approveResult, f.approveErr
}

func (f *fakeStore) GetAppealByOrder(context.Context, int64, string) (AppealView, error) {
	return f.appeal, f.appealErr
}

func (f *fakeStore) RejectAppeal(_ context.Context, _ int64, _ int64, reason string) (bool, error) {
	f.rejected = true
	f.rejectReason = reason
	return f.rejectResult, f.rejectErr
}

type fakeAuthProvider struct {
	identity auth.Identity
	found    bool
}

func (f fakeAuthProvider) RequireIdentity(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if !f.found {
			httpapi.WriteError(w, r, http.StatusUnauthorized, httpapi.CodeUnauthorized, "session is missing or expired", nil)
			return
		}
		next(w, r.WithContext(auth.WithIdentity(r.Context(), f.identity)))
	}
}

func (f fakeAuthProvider) RequireRole(role string, next http.HandlerFunc) http.HandlerFunc {
	return f.RequireIdentity(next)
}

func (f fakeAuthProvider) RequireAdminWrite(next http.HandlerFunc) http.HandlerFunc {
	return f.RequireRole(auth.RoleAdmin, next)
}

func newFixture(t *testing.T, identity auth.Identity, found bool) *httpapi.Server {
	t.Helper()
	store := &fakeStore{}
	service, err := NewService(store)
	if err != nil {
		t.Fatalf("NewService() error = %v", err)
	}
	handlers, err := NewHandlers(service, fakeAuthProvider{identity: identity, found: found}, fakeAuthProvider{identity: identity, found: found})
	if err != nil {
		t.Fatalf("NewHandlers() error = %v", err)
	}
	server := httpapi.NewServer(config.Config{RequestIDHeader: "X-Request-ID"}, slog.New(slog.NewTextHandler(io.Discard, nil)))
	handlers.Register(server)
	server.SetReady(true)
	return server
}

func TestReviewCreateValidation(t *testing.T) {
	server := newFixture(t, auth.Identity{ID: 7, Role: auth.RoleUser, Status: auth.StatusActive}, true)

	cases := []struct {
		name string
		body string
		want int
	}{
		{"zero stars", `{"stars":0,"comment":"好"}`, http.StatusBadRequest},
		{"six stars", `{"stars":6,"comment":"好"}`, http.StatusBadRequest},
		{"empty comment", `{"stars":5,"comment":"  "}`, http.StatusBadRequest},
		{"valid", `{"stars":5,"comment":"充电很快"}`, http.StatusCreated},
	}
	for _, testCase := range cases {
		request := httptest.NewRequest(http.MethodPost, "/api/v1/orders/ORD20260915120000aaaa/review", strings.NewReader(testCase.body))
		recorder := httptest.NewRecorder()
		server.Handler().ServeHTTP(recorder, request)
		if recorder.Code != testCase.want {
			t.Errorf("%s: status = %d, want %d", testCase.name, recorder.Code, testCase.want)
		}
	}
}

func TestWallRequiresAuthAndValidStation(t *testing.T) {
	anon := newFixture(t, auth.Identity{}, false)
	recorder := httptest.NewRecorder()
	anon.Handler().ServeHTTP(recorder, httptest.NewRequest(http.MethodGet, "/api/v1/stations/1/reviews", nil))
	if recorder.Code != http.StatusUnauthorized {
		t.Fatalf("anonymous wall status = %d", recorder.Code)
	}

	authed := newFixture(t, auth.Identity{ID: 7, Role: auth.RoleUser, Status: auth.StatusActive}, true)
	recorder2 := httptest.NewRecorder()
	authed.Handler().ServeHTTP(recorder2, httptest.NewRequest(http.MethodGet, "/api/v1/stations/notanumber/reviews", nil))
	if recorder2.Code != http.StatusBadRequest {
		t.Fatalf("bad station status = %d", recorder2.Code)
	}
}

func TestAppealApprovalEndpoint(t *testing.T) {
	admin := newFixture(t, auth.Identity{ID: 2, Role: auth.RoleAdmin, AdminRole: auth.AdminRoleOperator, Status: auth.StatusActive}, true)

	recorder := httptest.NewRecorder()
	admin.Handler().ServeHTTP(recorder, httptest.NewRequest(http.MethodPost, "/api/v1/admin/appeals/1/approve", nil))
	if recorder.Code != http.StatusOK {
		t.Fatalf("approve status = %d body = %s", recorder.Code, recorder.Body.String())
	}
	var payload map[string]any
	if err := json.NewDecoder(recorder.Body).Decode(&payload); err != nil {
		t.Fatalf("decode: %v", err)
	}
	data := payload["data"].(map[string]any)
	if data["status"] != AppealApproved {
		t.Fatalf("status = %v", data["status"])
	}
}

// UC-U-09: 重复审核不得重复记账 — the second decision succeeds as a no-op
// and reports the appeal's current state instead of conflicting.
func TestApproveDuplicateDecisionIsNoOpSuccess(t *testing.T) {
	store := &fakeStore{}
	service, err := NewService(store)
	if err != nil {
		t.Fatalf("NewService() error = %v", err)
	}
	// The store reports "already approved" (false) for both calls; the
	// service must turn that into a successful no-op.
	if err := service.Approve(context.Background(), 1, 2); err != nil {
		t.Fatalf("first Approve() error = %v", err)
	}
	if err := service.Approve(context.Background(), 1, 2); err != nil {
		t.Fatalf("duplicate Approve() error = %v, want a no-op success", err)
	}

	admin := newFixture(t, auth.Identity{ID: 2, Role: auth.RoleAdmin, AdminRole: auth.AdminRoleOperator, Status: auth.StatusActive}, true)
	for i := 0; i < 2; i++ {
		recorder := httptest.NewRecorder()
		admin.Handler().ServeHTTP(recorder, httptest.NewRequest(http.MethodPost, "/api/v1/admin/appeals/1/approve", nil))
		if recorder.Code != http.StatusOK {
			t.Fatalf("duplicate approve status = %d body = %s, want 200 with the current result", recorder.Code, recorder.Body.String())
		}
	}
}

func TestGetReviewEndpoint(t *testing.T) {
	user := auth.Identity{ID: 7, Role: auth.RoleUser, Status: auth.StatusActive}

	store := &fakeStore{review: ReviewView{
		OrderNo: "ORD20260915120000aaaa", Stars: 5, Comment: "充电很快",
		CreatedAt: time.Date(2026, 9, 15, 12, 0, 0, 0, time.UTC),
	}}
	service, err := NewService(store)
	if err != nil {
		t.Fatalf("NewService() error = %v", err)
	}
	handlers, err := NewHandlers(service, fakeAuthProvider{identity: user, found: true}, fakeAuthProvider{identity: user, found: true})
	if err != nil {
		t.Fatalf("NewHandlers() error = %v", err)
	}
	server := httpapi.NewServer(config.Config{RequestIDHeader: "X-Request-ID"}, slog.New(slog.NewTextHandler(io.Discard, nil)))
	handlers.Register(server)
	server.SetReady(true)

	recorder := httptest.NewRecorder()
	server.Handler().ServeHTTP(recorder, httptest.NewRequest(http.MethodGet, "/api/v1/orders/ORD20260915120000aaaa/review", nil))
	if recorder.Code != http.StatusOK {
		t.Fatalf("get review status = %d body = %s", recorder.Code, recorder.Body.String())
	}
	var payload map[string]any
	if err := json.NewDecoder(recorder.Body).Decode(&payload); err != nil {
		t.Fatalf("decode: %v", err)
	}
	data := payload["data"].(map[string]any)
	if data["orderNo"] != "ORD20260915120000aaaa" || data["stars"] != float64(5) {
		t.Fatalf("data = %v", data)
	}
	// Review.createdAt is required by the contract: it must be present and
	// carry the stored creation time.
	if createdAt, _ := data["createdAt"].(string); createdAt == "" {
		t.Fatalf("data.createdAt missing or empty: %v", data)
	}

	missing := &fakeStore{reviewErr: ErrNotFound}
	missingService, err := NewService(missing)
	if err != nil {
		t.Fatalf("NewService() error = %v", err)
	}
	missingHandlers, err := NewHandlers(missingService, fakeAuthProvider{identity: user, found: true}, fakeAuthProvider{identity: user, found: true})
	if err != nil {
		t.Fatalf("NewHandlers() error = %v", err)
	}
	missingServer := httpapi.NewServer(config.Config{RequestIDHeader: "X-Request-ID"}, slog.New(slog.NewTextHandler(io.Discard, nil)))
	missingHandlers.Register(missingServer)
	missingServer.SetReady(true)
	recorder2 := httptest.NewRecorder()
	missingServer.Handler().ServeHTTP(recorder2, httptest.NewRequest(http.MethodGet, "/api/v1/orders/ORD20260915120000aaaa/review", nil))
	if recorder2.Code != http.StatusNotFound {
		t.Fatalf("missing review status = %d body = %s, want 404", recorder2.Code, recorder2.Body.String())
	}
}

var _ = errors.New

// UC-U-09 follow-up: an appeal could only be closed by approving, which cancels
// the order and refunds. Rejection dismisses it with a reason and touches
// neither the order nor the wallet.
func TestAppealRejectionEndpoint(t *testing.T) {
	admin := newFixture(t, auth.Identity{ID: 2, Role: auth.RoleAdmin, AdminRole: auth.AdminRoleOperator, Status: auth.StatusActive}, true)

	recorder := httptest.NewRecorder()
	request := httptest.NewRequest(http.MethodPost, "/api/v1/admin/appeals/1/reject",
		strings.NewReader(`{"reason":"计量与设备记录一致，申诉不成立"}`))
	admin.Handler().ServeHTTP(recorder, request)
	if recorder.Code != http.StatusOK {
		t.Fatalf("reject status = %d body = %s", recorder.Code, recorder.Body.String())
	}

	var payload map[string]any
	if err := json.NewDecoder(recorder.Body).Decode(&payload); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if payload["data"] == nil {
		t.Fatalf("reject response = %s, want the current appeal state", recorder.Body.String())
	}
}

func TestAppealRejectValidationAndErrors(t *testing.T) {
	admin := newFixture(t, auth.Identity{ID: 2, Role: auth.RoleAdmin, AdminRole: auth.AdminRoleOperator, Status: auth.StatusActive}, true)

	// Empty reason: the operator has to say why the appeal is dismissed, because
	// the customer sees it.
	recorder := httptest.NewRecorder()
	admin.Handler().ServeHTTP(recorder, httptest.NewRequest(http.MethodPost, "/api/v1/admin/appeals/1/reject",
		strings.NewReader(`{"reason":"   "}`)))
	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("blank reason status = %d, want 400", recorder.Code)
	}

	// A malformed appeal id must not reach the store.
	recorder = httptest.NewRecorder()
	admin.Handler().ServeHTTP(recorder, httptest.NewRequest(http.MethodPost, "/api/v1/admin/appeals/abc/reject",
		strings.NewReader(`{"reason":"理由"}`)))
	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("bad appeal id status = %d, want 400", recorder.Code)
	}

	// GET on an admin-only action is a method error, not a silent success.
	recorder = httptest.NewRecorder()
	admin.Handler().ServeHTTP(recorder, httptest.NewRequest(http.MethodGet, "/api/v1/admin/appeals/1/reject", nil))
	if recorder.Code != http.StatusMethodNotAllowed {
		t.Fatalf("GET reject status = %d, want 405", recorder.Code)
	}
}

// The customer has to be able to read the outcome of their own appeal: the
// endpoint is the same order sub-resource the appeal is filed on, and a missing
// appeal is a 404 (the shape the review resource already uses).
func TestGetOwnAppealEndpoint(t *testing.T) {
	user := newFixture(t, auth.Identity{ID: 7, Role: auth.RoleUser, Status: auth.StatusActive}, true)

	recorder := httptest.NewRecorder()
	user.Handler().ServeHTTP(recorder, httptest.NewRequest(http.MethodGet, "/api/v1/orders/ORD20260915120000aaaa/appeal", nil))
	if recorder.Code != http.StatusOK {
		t.Fatalf("GET appeal status = %d body = %s", recorder.Code, recorder.Body.String())
	}

	anon := newFixture(t, auth.Identity{}, false)
	recorder = httptest.NewRecorder()
	anon.Handler().ServeHTTP(recorder, httptest.NewRequest(http.MethodGet, "/api/v1/orders/ORD20260915120000aaaa/appeal", nil))
	if recorder.Code != http.StatusUnauthorized {
		t.Fatalf("anonymous GET appeal status = %d, want 401", recorder.Code)
	}
}
