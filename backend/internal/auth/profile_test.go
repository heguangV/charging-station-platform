package auth

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"strings"
	"testing"
	"time"
)

type failingRevocationStore struct {
	*InMemorySessionStore
	err error
}

func (s *failingRevocationStore) RevokeAllForUser(context.Context, int64) error {
	return s.err
}

func TestMaskPhone(t *testing.T) {
	if got := MaskPhone("13812345678"); got != "138****5678" {
		t.Fatalf("MaskPhone = %q, want 138****5678", got)
	}
	if got := MaskPhone("short"); got != "***" {
		t.Fatalf("MaskPhone(short) = %q, want ***", got)
	}
}

func TestIsValidNickname(t *testing.T) {
	if !IsValidNickname("开发用户") {
		t.Fatal("valid nickname rejected")
	}
	if !IsValidNickname(strings.Repeat("中", 20)) {
		t.Fatal("20-character Unicode nickname rejected")
	}
	if IsValidNickname("") || IsValidNickname("   ") || IsValidNickname(strings.Repeat("中", 21)) {
		t.Fatal("invalid nickname accepted")
	}
}

func TestProfileAndDeletionFlow(t *testing.T) {
	f := newHandlerFixture(t)
	server := f.server()

	// login
	recorder, _ := doJSON(t, server.Handler(), http.MethodPost, "/api/v1/auth/user/login",
		`{"account":"13800000001","password":"`+testPassword+`"}`, nil)
	var login envelope
	if err := json.NewDecoder(recorder.Body).Decode(&login); err != nil {
		t.Fatalf("decode login: %v", err)
	}
	token := login.Data["accessToken"].(string)
	headers := map[string]string{"Authorization": "Bearer " + token}

	// profile view
	recorder, payload := doJSON(t, server.Handler(), http.MethodGet, "/api/v1/me/profile", "", headers)
	if recorder.Code != http.StatusOK {
		t.Fatalf("profile status = %d body = %s", recorder.Code, recorder.Body.String())
	}
	profile := payload.Data
	if profile["phoneMasked"] != "138****0001" {
		t.Fatalf("phoneMasked = %v", profile["phoneMasked"])
	}

	// nickname update
	recorder, payload = doJSON(t, server.Handler(), http.MethodPut, "/api/v1/me/profile",
		`{"displayName":"新昵称"}`, headers)
	if recorder.Code != http.StatusOK {
		t.Fatalf("update status = %d body = %s", recorder.Code, recorder.Body.String())
	}
	profile = payload.Data
	if profile["displayName"] != "新昵称" {
		t.Fatalf("displayName = %v", profile["displayName"])
	}

	// invalid nickname
	recorder, payload = doJSON(t, server.Handler(), http.MethodPut, "/api/v1/me/profile",
		`{"displayName":"   "}`, headers)
	if recorder.Code != http.StatusBadRequest || payload.Code != 1 {
		t.Fatalf("blank nickname: status = %d code = %v", recorder.Code, payload.Code)
	}

	// deletion: 204 and the session is revoked
	recorder, _ = doJSON(t, server.Handler(), http.MethodDelete, "/api/v1/me", "", headers)
	if recorder.Code != http.StatusNoContent {
		t.Fatalf("delete status = %d body = %s", recorder.Code, recorder.Body.String())
	}
	recorder, _ = doJSON(t, server.Handler(), http.MethodGet, "/api/v1/me", "", headers)
	if recorder.Code != http.StatusUnauthorized {
		t.Fatalf("me after deletion status = %d, want 401", recorder.Code)
	}
}

func TestFreezeRevokesSessionsAndBlocksLogin(t *testing.T) {
	f := newHandlerFixture(t)
	// Administrator and user IDs belong to separate account domains. Keep
	// them equal to catch accidental cross-domain "self" comparisons.
	f.reader.admin = &AdminAccount{
		ID: 7, Username: "chief", Role: AdminRoleSuper, PasswordHash: hashForTest(t, testPassword), Status: StatusActive,
	}
	server := f.server()

	recorder, _ := doJSON(t, server.Handler(), http.MethodPost, "/api/v1/auth/user/login",
		`{"account":"13800000001","password":"`+testPassword+`"}`, nil)
	var login envelope
	_ = json.NewDecoder(recorder.Body).Decode(&login)
	token := login.Data["accessToken"].(string)
	headers := map[string]string{"Authorization": "Bearer " + token}

	// The admin freezes the user through the protected HTTP route. Sessions
	// die immediately even though both account domains happen to use ID 7.
	adminRecorder, adminLogin := doJSON(t, server.Handler(), http.MethodPost, "/api/v1/auth/admin/login",
		`{"account":"chief","password":"`+testPassword+`"}`, nil)
	if adminRecorder.Code != http.StatusOK {
		t.Fatalf("admin login status = %d body = %s", adminRecorder.Code, adminRecorder.Body.String())
	}
	adminHeaders := map[string]string{"Authorization": "Bearer " + adminLogin.Data["accessToken"].(string)}
	recorder, payload := doJSON(t, server.Handler(), http.MethodPost, "/api/v1/admin/users/7/freeze", "", adminHeaders)
	if recorder.Code != http.StatusOK {
		t.Fatalf("freeze status = %d body = %s", recorder.Code, recorder.Body.String())
	}
	if payload.Data["status"] != StatusDisable || f.reader.user.Status != StatusDisable {
		t.Fatalf("freeze response/account status = %v/%s, want %s", payload.Data["status"], f.reader.user.Status, StatusDisable)
	}

	recorder, _ = doJSON(t, server.Handler(), http.MethodGet, "/api/v1/me", "", headers)
	if recorder.Code != http.StatusUnauthorized {
		t.Fatalf("me after freeze status = %d, want 401", recorder.Code)
	}

	// BR-07: the frozen user cannot log in again.
	recorder, payload = doJSON(t, server.Handler(), http.MethodPost, "/api/v1/auth/user/login",
		`{"account":"13800000001","password":"`+testPassword+`"}`, nil)
	if recorder.Code != http.StatusForbidden || payload.Code != 6 {
		t.Fatalf("frozen login: status = %d code = %v", recorder.Code, payload.Code)
	}

	// Unfreezing takes the opposite branch and restores login access.
	recorder, payload = doJSON(t, server.Handler(), http.MethodPost, "/api/v1/admin/users/7/unfreeze", "", adminHeaders)
	if recorder.Code != http.StatusOK {
		t.Fatalf("unfreeze status = %d body = %s", recorder.Code, recorder.Body.String())
	}
	if payload.Data["status"] != StatusActive || f.reader.user.Status != StatusActive {
		t.Fatalf("unfreeze response/account status = %v/%s, want %s", payload.Data["status"], f.reader.user.Status, StatusActive)
	}
	recorder, _ = doJSON(t, server.Handler(), http.MethodPost, "/api/v1/auth/user/login",
		`{"account":"13800000001","password":"`+testPassword+`"}`, nil)
	if recorder.Code != http.StatusOK {
		t.Fatalf("login after unfreeze status = %d body = %s", recorder.Code, recorder.Body.String())
	}
}

func TestUnfreezeLeavesAccountDisabledWhenSessionRevocationFails(t *testing.T) {
	account := &UserAccount{ID: 7, Phone: "13800000001", Status: StatusDisable}
	mutations := NewInMemoryAccountMutation(map[int64]*UserAccount{account.ID: account})
	revokeErr := errors.New("redis unavailable")
	sessions := &failingRevocationStore{
		InMemorySessionStore: NewInMemorySessionStore(time.Minute, nil),
		err:                  revokeErr,
	}
	reader := &fakeAccountReader{user: account}
	service, err := NewService(reader, reader, mutations, sessions,
		NewFixedWindowLimiter(10, time.Minute, nil), NewInMemorySMSCodeStore(nil), time.Minute, time.Hour)
	if err != nil {
		t.Fatalf("NewService() error = %v", err)
	}

	if err := service.UnfreezeUser(context.Background(), 1, account.ID); !errors.Is(err, revokeErr) {
		t.Fatalf("UnfreezeUser() error = %v, want %v", err, revokeErr)
	}
	if account.Status != StatusDisable {
		t.Fatalf("account status = %s, want %s", account.Status, StatusDisable)
	}
}
