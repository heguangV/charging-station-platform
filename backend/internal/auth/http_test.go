package auth

import (
	"bytes"
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

	"github.com/heguangV/charging-station-platform/backend/internal/config"
	"github.com/heguangV/charging-station-platform/backend/internal/httpapi"
)

type handlerFixture struct {
	handlers *Handlers
	reader   *fakeAccountReader
	codes    SMSCodeStore
}

func newHandlerFixture(t *testing.T) handlerFixture {
	t.Helper()
	reader := &fakeAccountReader{user: &UserAccount{
		ID: 7, Phone: "13800000001", DisplayName: "开发用户", PasswordHash: hashForTest(t, testPassword), Status: StatusActive,
	}}
	mutations := NewInMemoryAccountMutation(map[int64]*UserAccount{reader.user.ID: reader.user})
	codes := NewInMemorySMSCodeStore(nil)
	service, err := NewService(
		reader,
		reader,
		mutations,
		NewInMemorySessionStore(time.Minute, nil),
		NewFixedWindowLimiter(5, time.Minute, nil),
		codes,
		time.Minute, time.Hour,
	)
	if err != nil {
		t.Fatalf("NewService() error = %v", err)
	}
	service.SMSMock = true
	handlers, err := NewHandlers(service)
	if err != nil {
		t.Fatalf("NewHandlers() error = %v", err)
	}
	return handlerFixture{handlers: handlers, reader: reader, codes: codes}
}

func (f handlerFixture) server() *httpapi.Server {
	server := httpapi.NewServer(config.Config{RequestIDHeader: "X-Request-ID"}, slog.New(slog.NewTextHandler(io.Discard, nil)))
	f.handlers.Register(server)
	server.SetReady(true)
	return server
}

type envelope struct {
	Success bool           `json:"success"`
	Code    int            `json:"code"`
	Message string         `json:"message"`
	Data    map[string]any `json:"data"`
	TraceID string         `json:"traceId"`
}

func doJSON(t *testing.T, handler http.Handler, method, path, body string, headers map[string]string) (*httptest.ResponseRecorder, envelope) {
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

	// Decode from a snapshot: decoding straight from recorder.Body would
	// consume the buffer and break later assertions that read it again.
	bodyBytes := recorder.Body.Bytes()
	var payload envelope
	if len(bodyBytes) > 0 {
		if err := json.NewDecoder(bytes.NewReader(bodyBytes)).Decode(&payload); err != nil {
			t.Fatalf("decode envelope: %v (body %q)", err, recorder.Body.String())
		}
	}
	return recorder, payload
}

func TestUserLoginSuccessEnvelope(t *testing.T) {
	fixture := newHandlerFixture(t)
	server := fixture.server()

	recorder, payload := doJSON(t, server.Handler(), http.MethodPost, "/api/v1/auth/user/login",
		`{"account":"13800000001","password":"`+testPassword+`"}`, nil)

	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d, body = %s", recorder.Code, recorder.Body.String())
	}
	if !payload.Success || payload.Code != httpapi.CodeOK {
		t.Fatalf("envelope = %#v", payload)
	}
	if payload.Data["accessToken"] == "" || payload.Data["accessToken"] == nil {
		t.Fatalf("accessToken missing: %#v", payload.Data)
	}
	identity, ok := payload.Data["identity"].(map[string]any)
	if !ok {
		t.Fatalf("identity missing: %#v", payload.Data)
	}
	if identity["id"].(float64) != 7 || identity["role"] != RoleUser || identity["displayName"] != "开发用户" {
		t.Fatalf("identity = %#v", identity)
	}
	if _, err := time.Parse(time.RFC3339, payload.Data["expiresAt"].(string)); err != nil {
		t.Fatalf("expiresAt = %v is not RFC3339", payload.Data["expiresAt"])
	}
}

func TestAdminLoginRequiresAdminReader(t *testing.T) {
	fixture := newHandlerFixture(t)
	server := fixture.server()

	recorder, payload := doJSON(t, server.Handler(), http.MethodPost, "/api/v1/auth/admin/login",
		`{"account":"admin","password":"`+testPassword+`"}`, nil)

	// The fixture has no admin account; admin login must not fall back to
	// the user table.
	if recorder.Code != http.StatusUnauthorized || payload.Code != httpapi.CodeUnauthorized {
		t.Fatalf("status = %d code = %d, want 401/%d", recorder.Code, payload.Code, httpapi.CodeUnauthorized)
	}
}

func TestLoginValidationErrors(t *testing.T) {
	fixture := newHandlerFixture(t)
	server := fixture.server()

	cases := []struct {
		name string
		body string
	}{
		{"malformed json", `{"account":`},
		{"unknown field", `{"account":"13800000001","password":"` + testPassword + `","rememberMe":true}`},
		{"second json value", `{"account":"13800000001","password":"` + testPassword + `"} {}`},
		{"short account", `{"account":"a","password":"` + testPassword + `"}`},
		{"short password", `{"account":"13800000001","password":"short"}`},
	}
	for _, testCase := range cases {
		recorder, payload := doJSON(t, server.Handler(), http.MethodPost, "/api/v1/auth/user/login", testCase.body, nil)
		if recorder.Code != http.StatusBadRequest || payload.Code != httpapi.CodeInvalidArgument {
			t.Errorf("%s: status = %d code = %d, want 400/%d", testCase.name, recorder.Code, payload.Code, httpapi.CodeInvalidArgument)
		}
	}
}

func TestLoginInvalidCredentialsAndFrozenAndUnavailable(t *testing.T) {
	fixture := newHandlerFixture(t)
	server := fixture.server()
	headers := func(body string) map[string]string { return nil }

	recorder, payload := doJSON(t, server.Handler(), http.MethodPost, "/api/v1/auth/user/login",
		`{"account":"13800000001","password":"Wrong-Password-1"}`, headers(""))
	if recorder.Code != http.StatusUnauthorized || payload.Code != httpapi.CodeUnauthorized || payload.TraceID == "" {
		t.Fatalf("wrong password: status = %d code = %d traceId = %q", recorder.Code, payload.Code, payload.TraceID)
	}

	fixture.reader.user.Status = StatusDisable
	recorder, payload = doJSON(t, server.Handler(), http.MethodPost, "/api/v1/auth/user/login",
		`{"account":"13800000001","password":"`+testPassword+`"}`, nil)
	if recorder.Code != http.StatusForbidden || payload.Code != httpapi.CodeUserFrozen {
		t.Fatalf("frozen: status = %d code = %d", recorder.Code, payload.Code)
	}
	fixture.reader.user.Status = StatusActive
}

func TestLoginRateLimitedIncludesRetryAfter(t *testing.T) {
	fixture := newHandlerFixture(t)
	server := fixture.server()
	body := `{"account":"13800000001","password":"Wrong-Password-1"}`

	var gotRetry float64
	for attempt := 0; attempt < 6; attempt++ {
		recorder, payload := doJSON(t, server.Handler(), http.MethodPost, "/api/v1/auth/user/login", body, nil)
		if attempt < 5 {
			continue
		}
		if recorder.Code != http.StatusTooManyRequests || payload.Code != httpapi.CodeRateLimited {
			t.Fatalf("status = %d code = %d, want 429/%d", recorder.Code, payload.Code, httpapi.CodeRateLimited)
		}
		gotRetry = payload.Data["retryAfterSec"].(float64)
	}
	if gotRetry < 1 || gotRetry > 60 {
		t.Fatalf("retryAfterSec = %v, want within the window", gotRetry)
	}
}

func TestMeAndLogoutRequireBearerToken(t *testing.T) {
	fixture := newHandlerFixture(t)
	server := fixture.server()

	recorder, payload := doJSON(t, server.Handler(), http.MethodGet, "/api/v1/me", "", nil)
	if recorder.Code != http.StatusUnauthorized || payload.Code != httpapi.CodeUnauthorized {
		t.Fatalf("anonymous me: status = %d code = %d", recorder.Code, payload.Code)
	}

	recorder, _ = doJSON(t, server.Handler(), http.MethodPost, "/api/v1/auth/user/login",
		`{"account":"13800000001","password":"`+testPassword+`"}`, nil)
	var login envelope
	if err := json.NewDecoder(recorder.Body).Decode(&login); err != nil {
		t.Fatalf("decode login: %v", err)
	}
	token := login.Data["accessToken"].(string)
	authHeader := map[string]string{"Authorization": "Bearer " + token}

	recorder, payload = doJSON(t, server.Handler(), http.MethodGet, "/api/v1/me", "", authHeader)
	if recorder.Code != http.StatusOK {
		t.Fatalf("me with token: status = %d body = %s", recorder.Code, recorder.Body.String())
	}
	if payload.Data["role"] != RoleUser || payload.Data["id"].(float64) != 7 {
		t.Fatalf("me identity = %#v", payload.Data)
	}

	// HTTP authentication scheme names are case-insensitive.
	recorder, _ = doJSON(t, server.Handler(), http.MethodGet, "/api/v1/me", "",
		map[string]string{"Authorization": "bearer " + token})
	if recorder.Code != http.StatusOK {
		t.Fatalf("lowercase bearer status = %d, want 200", recorder.Code)
	}

	recorder, _ = doJSON(t, server.Handler(), http.MethodPost, "/api/v1/auth/logout", "", authHeader)
	if recorder.Code != http.StatusNoContent {
		t.Fatalf("logout status = %d, want 204", recorder.Code)
	}

	// The token is dead: /me rejects it and a second logout stays 204.
	recorder, _ = doJSON(t, server.Handler(), http.MethodGet, "/api/v1/me", "", authHeader)
	if recorder.Code != http.StatusUnauthorized {
		t.Fatalf("me after logout status = %d, want 401", recorder.Code)
	}
	recorder, _ = doJSON(t, server.Handler(), http.MethodPost, "/api/v1/auth/logout", "", authHeader)
	if recorder.Code != http.StatusNoContent {
		t.Fatalf("idempotent logout status = %d, want 204", recorder.Code)
	}
}

func TestAuthenticatedRequestRejectsAccountFrozenAfterLogin(t *testing.T) {
	fixture := newHandlerFixture(t)
	server := fixture.server()
	_, login := doJSON(t, server.Handler(), http.MethodPost, "/api/v1/auth/user/login",
		`{"account":"13800000001","password":"`+testPassword+`"}`, nil)
	token := login.Data["accessToken"].(string)
	fixture.reader.user.Status = StatusDisable

	recorder, payload := doJSON(t, server.Handler(), http.MethodGet, "/api/v1/me", "",
		map[string]string{"Authorization": "Bearer " + token})
	if recorder.Code != http.StatusForbidden || payload.Code != httpapi.CodeUserFrozen {
		t.Fatalf("status = %d code = %d, want 403/%d", recorder.Code, payload.Code, httpapi.CodeUserFrozen)
	}
}

func TestAuthenticatedRequestReturnsServiceUnavailableWhenAccountLookupFails(t *testing.T) {
	fixture := newHandlerFixture(t)
	server := fixture.server()
	_, login := doJSON(t, server.Handler(), http.MethodPost, "/api/v1/auth/user/login",
		`{"account":"13800000001","password":"`+testPassword+`"}`, nil)
	token := login.Data["accessToken"].(string)
	headers := map[string]string{"Authorization": "Bearer " + token}

	originalMutations := fixture.handlers.service.mutations
	fixture.handlers.service.mutations = failingProfileMutation{
		AccountMutation: originalMutations,
		err:             errors.New("database down"),
	}
	recorder, payload := doJSON(t, server.Handler(), http.MethodGet, "/api/v1/me", "", headers)
	if recorder.Code != http.StatusServiceUnavailable || payload.Code != httpapi.CodeDatabaseError {
		t.Fatalf("status = %d code = %d, want 503/%d", recorder.Code, payload.Code, httpapi.CodeDatabaseError)
	}

	fixture.handlers.service.mutations = originalMutations
	recorder, _ = doJSON(t, server.Handler(), http.MethodGet, "/api/v1/me", "", headers)
	if recorder.Code != http.StatusOK {
		t.Fatalf("request after recovery status = %d, want 200", recorder.Code)
	}
}

func TestWrongMethodKeepsEnvelopeAndAllowHeader(t *testing.T) {
	fixture := newHandlerFixture(t)
	server := fixture.server()

	recorder, payload := doJSON(t, server.Handler(), http.MethodGet, "/api/v1/auth/user/login", "", nil)
	if recorder.Code != http.StatusMethodNotAllowed || payload.Code != httpapi.CodeMethodNotAllowed {
		t.Fatalf("status = %d code = %d, want 405/%d", recorder.Code, payload.Code, httpapi.CodeMethodNotAllowed)
	}
	if recorder.Header().Get("Allow") != http.MethodPost {
		t.Fatalf("Allow = %q, want POST", recorder.Header().Get("Allow"))
	}
}

func TestRequireRoleBlocksUsersFromAdminRoutes(t *testing.T) {
	fixture := newHandlerFixture(t)
	server := fixture.server()

	protected := func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
	}
	server.Register("/api/v1/admin-only", fixture.handlers.RequireRole(RoleAdmin, protected))

	recorder, payload := doJSON(t, server.Handler(), http.MethodGet, "/api/v1/admin-only", "", nil)
	if recorder.Code != http.StatusUnauthorized {
		t.Fatalf("anonymous status = %d, want 401", recorder.Code)
	}

	_, login := doJSON(t, server.Handler(), http.MethodPost, "/api/v1/auth/user/login",
		`{"account":"13800000001","password":"`+testPassword+`"}`, nil)
	userToken := login.Data["accessToken"].(string)
	recorder, payload = doJSON(t, server.Handler(), http.MethodGet, "/api/v1/admin-only", "",
		map[string]string{"Authorization": "Bearer " + userToken})
	if recorder.Code != http.StatusForbidden || payload.Code != httpapi.CodeForbidden {
		t.Fatalf("user on admin route: status = %d code = %d, want 403/%d", recorder.Code, payload.Code, httpapi.CodeForbidden)
	}
}

func TestSMSLoginFlowEndpoints(t *testing.T) {
	fixture := newHandlerFixture(t)
	server := fixture.server()

	// Issue a code in mock mode: the response carries it (simulated SMS).
	recorder, payload := doJSON(t, server.Handler(), http.MethodPost, "/api/v1/auth/user/sms/code",
		`{"phone":"13611112222"}`, nil)
	if recorder.Code != http.StatusOK {
		t.Fatalf("sms code status = %d body = %s", recorder.Code, recorder.Body.String())
	}
	code := payload.Data["code"].(string)
	if len(code) != 6 {
		t.Fatalf("mock sms code = %q, want 6 digits", code)
	}

	// Login with the code registers the unknown phone automatically.
	recorder, payload = doJSON(t, server.Handler(), http.MethodPost, "/api/v1/auth/user/login/sms",
		`{"phone":"13611112222","code":"`+code+`"}`, nil)
	if recorder.Code != http.StatusOK {
		t.Fatalf("sms login status = %d body = %s", recorder.Code, recorder.Body.String())
	}
	identity := payload.Data["identity"].(map[string]any)
	if identity["role"] != RoleUser || identity["displayName"] != "用户2222" {
		t.Fatalf("sms identity = %#v", identity)
	}

	// The code is consumed: a second login with it fails as CODE_INVALID.
	recorder, payload = doJSON(t, server.Handler(), http.MethodPost, "/api/v1/auth/user/login/sms",
		`{"phone":"13611112222","code":"`+code+`"}`, nil)
	if recorder.Code != http.StatusUnprocessableEntity || payload.Code != 20 {
		t.Fatalf("replayed code: status = %d code = %v", recorder.Code, payload.Code)
	}

	// Immediate resend hits the cooldown (429 RATE_LIMITED).
	recorder, payload = doJSON(t, server.Handler(), http.MethodPost, "/api/v1/auth/user/sms/code",
		`{"phone":"13611112222"}`, nil)
	if recorder.Code != http.StatusTooManyRequests || payload.Code != httpapi.CodeRateLimited {
		t.Fatalf("cooldown: status = %d code = %v", recorder.Code, payload.Code)
	}

	// Invalid phone shapes are malformed requests.
	recorder, payload = doJSON(t, server.Handler(), http.MethodPost, "/api/v1/auth/user/sms/code",
		`{"phone":"not-a-phone"}`, nil)
	if recorder.Code != http.StatusBadRequest || payload.Code != httpapi.CodeInvalidArgument {
		t.Fatalf("bad phone: status = %d code = %v", recorder.Code, payload.Code)
	}
}

func TestSMSCodeRequestFailsWithoutSimulatedDelivery(t *testing.T) {
	fixture := newHandlerFixture(t)
	fixture.handlers.service.SMSMock = false
	server := fixture.server()

	recorder, payload := doJSON(t, server.Handler(), http.MethodPost, "/api/v1/auth/user/sms/code",
		`{"phone":"13611112222"}`, nil)
	if recorder.Code != http.StatusServiceUnavailable || payload.Code != 12 {
		t.Fatalf("no provider: status = %d code = %v", recorder.Code, payload.Code)
	}
	// No code was stored either.
	if _, found := fixture.handlers.service.codes.(*InMemorySMSCodeStore).Peek("13611112222"); found {
		t.Fatal("code stored although no provider can deliver it")
	}
}

func TestAdminRoleExposedByIdentity(t *testing.T) {
	fixture := newHandlerFixture(t)
	fixture.reader.admin = &AdminAccount{
		ID: 2, Username: "chief", Role: AdminRoleSuper, PasswordHash: hashForTest(t, testPassword), Status: StatusActive,
	}
	server := fixture.server()

	recorder, payload := doJSON(t, server.Handler(), http.MethodPost, "/api/v1/auth/admin/login",
		`{"account":"chief","password":"`+testPassword+`"}`, nil)
	if recorder.Code != http.StatusOK {
		t.Fatalf("admin login status = %d body = %s", recorder.Code, recorder.Body.String())
	}
	identity := payload.Data["identity"].(map[string]any)
	if identity["role"] != RoleAdmin || identity["adminRole"] != AdminRoleSuper {
		t.Fatalf("admin identity = %#v", identity)
	}

	token := payload.Data["accessToken"].(string)
	recorder, payload = doJSON(t, server.Handler(), http.MethodGet, "/api/v1/me", "",
		map[string]string{"Authorization": "Bearer " + token})
	if recorder.Code != http.StatusOK {
		t.Fatalf("me status = %d", recorder.Code)
	}
	if payload.Data["adminRole"] != AdminRoleSuper {
		t.Fatalf("me adminRole = %v", payload.Data["adminRole"])
	}

	adminHeaders := map[string]string{"Authorization": "Bearer " + token}
	for _, request := range []struct {
		method string
		path   string
		body   string
	}{
		{http.MethodPut, "/api/v1/me", `{"displayName":"cross-domain"}`},
		{http.MethodDelete, "/api/v1/me", ""},
		{http.MethodGet, "/api/v1/me/profile", ""},
	} {
		recorder, payload = doJSON(t, server.Handler(), request.method, request.path, request.body, adminHeaders)
		if recorder.Code != http.StatusForbidden || payload.Code != httpapi.CodeForbidden {
			t.Errorf("admin %s %s: status = %d code = %d, want 403/%d", request.method, request.path, recorder.Code, payload.Code, httpapi.CodeForbidden)
		}
	}
}

// TestRegisterEndpoint covers POST /api/v1/auth/user/register: a valid phone
// with a valid code creates the account and returns a session (201), a phone
// that already has an account is 409 ALREADY_EXISTS, a password outside the
// contract bounds is 400, and a wrong code is 422 CODE_INVALID with no account
// created at all.
func TestRegisterEndpoint(t *testing.T) {
	const phone = "13900000042"
	password := strings.Repeat("a", 12)

	post := func(f handlerFixture, body string) (*httptest.ResponseRecorder, envelope) {
		t.Helper()
		recorder := httptest.NewRecorder()
		request := httptest.NewRequest(http.MethodPost, "/api/v1/auth/user/register", bytes.NewBufferString(body))
		request.Header.Set("Content-Type", "application/json")
		f.server().Handler().ServeHTTP(recorder, request)
		var payload envelope
		if err := json.Unmarshal(recorder.Body.Bytes(), &payload); err != nil {
			t.Fatalf("decode %q: %v", recorder.Body.String(), err)
		}
		return recorder, payload
	}

	// Wrong code first: it must not create anything.
	f := newHandlerFixture(t)
	if err := f.codes.Issue(context.Background(), phone, "123456", time.Minute); err != nil {
		t.Fatalf("issue code: %v", err)
	}
	recorder, payload := post(f, `{"username":"新用户","phone":"`+phone+`","password":"`+password+`","smsCode":"000000"}`)
	if recorder.Code != http.StatusUnprocessableEntity || payload.Code != 20 {
		t.Fatalf("wrong code: status = %d code = %d", recorder.Code, payload.Code)
	}
	if len(f.reader.registerCalls) != 0 {
		t.Fatalf("a wrong code created an account: %#v", f.reader.registerCalls)
	}

	// Weak password: rejected before the code is consumed, so the caller can
	// retry with the same code.
	recorder, _ = post(f, `{"phone":"`+phone+`","password":"short","smsCode":"123456"}`)
	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("weak password status = %d body = %s", recorder.Code, recorder.Body.String())
	}
	if len(f.reader.registerCalls) != 0 {
		t.Fatal("a weak password created an account")
	}

	// Valid registration: 201 with a session, and the stored hash matches the
	// password the caller sent.
	recorder, payload = post(f, `{"username":"新用户","phone":"`+phone+`","password":"`+password+`","smsCode":"123456"}`)
	if recorder.Code != http.StatusCreated {
		t.Fatalf("register status = %d body = %s", recorder.Code, recorder.Body.String())
	}
	if payload.Data["accessToken"] == nil || payload.Data["accessToken"] == "" {
		t.Fatalf("register data = %#v, want a session", payload.Data)
	}
	if len(f.reader.registerCalls) != 1 {
		t.Fatalf("register calls = %d, want 1", len(f.reader.registerCalls))
	}
	call := f.reader.registerCalls[0]
	if call.phone != phone || call.displayName != "新用户" {
		t.Fatalf("register call = %#v", call)
	}
	if !VerifyPassword(password, call.passwordHash) {
		t.Fatal("the stored hash does not verify against the password that was sent")
	}

	// The same phone again: 409 ALREADY_EXISTS, not a second account. A fresh
	// code is required because the first one was consumed by the registration
	// (single use), which is also why an attacker cannot probe whether a phone
	// is registered without a code for that phone.
	if err := f.codes.Issue(context.Background(), phone, "654321", time.Minute); err != nil {
		t.Fatalf("issue second code: %v", err)
	}
	f.reader.registerErr = ErrAccountExists
	recorder, payload = post(f, `{"phone":"`+phone+`","password":"`+password+`","smsCode":"654321"}`)
	if recorder.Code != http.StatusConflict || payload.Code != 5 {
		t.Fatalf("duplicate: status = %d code = %d", recorder.Code, payload.Code)
	}

	// Missing fields never reach the register path.
	f.reader.registerErr = nil
	f.reader.registerCalls = nil
	recorder, _ = post(f, `{"phone":"","password":"`+password+`","smsCode":"123456"}`)
	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("missing phone status = %d", recorder.Code)
	}
	if len(f.reader.registerCalls) != 0 {
		t.Fatal("an invalid phone reached the writer")
	}
}
