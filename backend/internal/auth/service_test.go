package auth

import (
	"context"
	"errors"
	"testing"
	"time"
)

// fakeAccountReader serves canned accounts for service tests. It matches on
// the account identifier exactly like the real store would, and doubles as
// the account writer for SMS auto-registration.
// registerCall records one RegisterUser invocation.
type registerCall struct {
	phone        string
	displayName  string
	passwordHash string
}

type fakeAccountReader struct {
	registerCalls []registerCall
	registerErr   error
	user          *UserAccount
	admin         *AdminAccount
}

func (f *fakeAccountReader) FindUserByAccount(_ context.Context, account string) (*UserAccount, error) {
	if f.user != nil && (account == f.user.Phone || account == f.user.Email) {
		return f.user, nil
	}
	return nil, nil
}

func (f *fakeAccountReader) FindAdminByUsername(_ context.Context, username string) (*AdminAccount, error) {
	if f.admin != nil && username == f.admin.Username {
		return f.admin, nil
	}
	return nil, nil
}

func (f *fakeAccountReader) FindAdminByID(_ context.Context, adminID int64) (*AdminAccount, error) {
	if f.admin != nil && adminID == f.admin.ID {
		return f.admin, nil
	}
	return nil, nil
}

func (f *fakeAccountReader) EnsureUserWithWallet(_ context.Context, phone string) (UserAccount, error) {
	if f.user != nil && f.user.Phone == phone {
		return *f.user, nil
	}
	return UserAccount{ID: 99, Phone: phone, DisplayName: "用户" + phone[len(phone)-4:], Status: StatusActive}, nil
}

// failingAccountReader simulates a database outage.
type failingAccountReader struct{}

func (failingAccountReader) FindUserByAccount(context.Context, string) (*UserAccount, error) {
	return nil, errors.New("database down")
}

func (failingAccountReader) FindAdminByUsername(context.Context, string) (*AdminAccount, error) {
	return nil, errors.New("database down")
}

// RegisterUser records what a registration asked for and reports the configured
// conflict, so the service's ordering (validate, verify the code, then create)
// can be asserted without a database.
func (f *fakeAccountReader) RegisterUser(_ context.Context, phone, displayName, passwordHash string) (UserAccount, error) {
	f.registerCalls = append(f.registerCalls, registerCall{phone: phone, displayName: displayName, passwordHash: passwordHash})
	if f.registerErr != nil {
		return UserAccount{}, f.registerErr
	}
	return UserAccount{ID: 123, Phone: phone, DisplayName: displayName, PasswordHash: passwordHash, Status: StatusActive}, nil
}

func (failingAccountReader) RegisterUser(context.Context, string, string, string) (UserAccount, error) {
	return UserAccount{}, errors.New("writer is down")
}

func (failingAccountReader) EnsureUserWithWallet(context.Context, string) (UserAccount, error) {
	return UserAccount{}, errors.New("database down")
}

func hashForTest(t *testing.T, password string) string {
	t.Helper()
	hash, err := HashPasswordWithIterations(password, 1000)
	if err != nil {
		t.Fatalf("hash for test: %v", err)
	}
	return hash
}

func newTestService(t *testing.T, reader *fakeAccountReader, limiter LoginRateLimiter) *Service {
	t.Helper()
	return newTestServiceWithMutations(t, reader, limiter, NewInMemorySMSCodeStore(nil))
}

func newTestServiceWithMutations(t *testing.T, reader *fakeAccountReader, limiter LoginRateLimiter, codes *InMemorySMSCodeStore) *Service {
	t.Helper()
	mutations := NewInMemoryAccountMutation(map[int64]*UserAccount{})
	if reader.user != nil {
		mutations.accounts[reader.user.ID] = reader.user
	}
	if reader.admin != nil {
		// Admin identities are outside the user-account namespace; nothing
		// to mutate for them in profile tests.
		_ = reader.admin
	}
	service, err := NewService(reader, reader, mutations, NewInMemorySessionStore(time.Minute, nil), limiter, codes, time.Minute, time.Hour)
	if err != nil {
		t.Fatalf("NewService() error = %v", err)
	}
	return service
}

const testPassword = "Dev-Password-01"

func TestLoginUserSuccess(t *testing.T) {
	reader := &fakeAccountReader{user: &UserAccount{
		ID: 42, Phone: "13800000001", DisplayName: "开发用户", PasswordHash: hashForTest(t, testPassword), Status: StatusActive,
	}}
	service := newTestService(t, reader, NewFixedWindowLimiter(10, time.Minute, nil))

	result, err := service.Login(context.Background(), "13800000001", testPassword)
	if err != nil {
		t.Fatalf("Login() error = %v", err)
	}
	if result.Token == "" || len(result.Token) < 32 {
		t.Fatalf("token = %q, want an opaque random string", result.Token)
	}
	if result.Identity.ID != 42 || result.Identity.Role != RoleUser || result.Identity.DisplayName != "开发用户" {
		t.Fatalf("identity = %#v", result.Identity)
	}
	if !result.ExpiresAt.After(time.Now()) {
		t.Fatalf("expiresAt = %v, want a future deadline", result.ExpiresAt)
	}

	// The session resolves back to the same identity.
	identity, err := service.Identify(context.Background(), result.Token)
	if err != nil {
		t.Fatalf("Identify() error = %v", err)
	}
	if identity != result.Identity {
		t.Fatalf("session identity = %#v, want %#v", identity, result.Identity)
	}
}

func TestLoginAdminSuccessAndRole(t *testing.T) {
	reader := &fakeAccountReader{admin: &AdminAccount{
		ID: 1, Username: "admin", PasswordHash: hashForTest(t, testPassword), Status: StatusActive,
	}}
	service := newTestService(t, reader, NewFixedWindowLimiter(10, time.Minute, nil))

	result, err := service.LoginAdmin(context.Background(), "admin", testPassword)
	if err != nil {
		t.Fatalf("LoginAdmin() error = %v", err)
	}
	if result.Identity.Role != RoleAdmin || result.Identity.ID != 1 || result.Identity.DisplayName != "admin" {
		t.Fatalf("admin identity = %#v", result.Identity)
	}
	if !Authorize(result.Identity, RoleAdmin) {
		t.Fatal("admin identity fails admin authorization")
	}
	if Authorize(result.Identity, RoleUser) {
		t.Fatal("admin identity passes user authorization")
	}
}

func TestIdentifyRejectsAnAdministratorDisabledAfterLogin(t *testing.T) {
	reader := &fakeAccountReader{admin: &AdminAccount{
		ID: 7, Username: "operator", Role: AdminRoleOperator,
		PasswordHash: hashForTest(t, testPassword), Status: StatusActive,
	}}
	service := newTestService(t, reader, NewFixedWindowLimiter(10, time.Minute, nil))
	result, err := service.LoginAdmin(context.Background(), "operator", testPassword)
	if err != nil {
		t.Fatalf("LoginAdmin() error = %v", err)
	}

	reader.admin.Status = StatusDisable
	if _, err := service.Identify(context.Background(), result.Token); !errors.Is(err, ErrUnauthorized) {
		t.Fatalf("Identify() after disable error = %v, want ErrUnauthorized", err)
	}
	if _, err := service.Identify(context.Background(), result.Token); !errors.Is(err, ErrUnauthorized) {
		t.Fatalf("disabled session was not deleted: %v", err)
	}
}

func TestLoginUnknownAccountAndWrongPasswordAreEquivalent(t *testing.T) {
	reader := &fakeAccountReader{user: &UserAccount{
		ID: 1, PasswordHash: hashForTest(t, testPassword), Status: StatusActive,
	}}
	service := newTestService(t, reader, NewFixedWindowLimiter(100, time.Minute, nil))

	_, unknownErr := service.Login(context.Background(), "nobody", testPassword)
	_, wrongErr := service.Login(context.Background(), "13800000001", "Wrong-Password-1")
	if !errors.Is(unknownErr, ErrInvalidCredentials) || !errors.Is(wrongErr, ErrInvalidCredentials) {
		t.Fatalf("unknown account err = %v, wrong password err = %v", unknownErr, wrongErr)
	}
}

func TestLoginFrozenAccount(t *testing.T) {
	reader := &fakeAccountReader{user: &UserAccount{
		ID: 1, Phone: "13800000001", PasswordHash: hashForTest(t, testPassword), Status: StatusDisable,
	}}
	service := newTestService(t, reader, NewFixedWindowLimiter(100, time.Minute, nil))

	if _, err := service.Login(context.Background(), "13800000001", testPassword); !errors.Is(err, ErrUserFrozen) {
		t.Fatalf("Login() error = %v, want ErrUserFrozen", err)
	}
}

func TestLoginRateLimitBlocksAndReportsRetry(t *testing.T) {
	reader := &fakeAccountReader{user: &UserAccount{
		ID: 1, Phone: "13800000001", PasswordHash: hashForTest(t, testPassword), Status: StatusActive,
	}}
	service := newTestService(t, reader, NewFixedWindowLimiter(3, time.Minute, nil))

	for attempt := 0; attempt < 3; attempt++ {
		// Wrong passwords still advance the counter: brute force must not
		// depend on succeeding.
		if _, err := service.Login(context.Background(), "13800000001", "Wrong-Password-1"); !errors.Is(err, ErrInvalidCredentials) {
			t.Fatalf("attempt %d error = %v", attempt, err)
		}
	}

	_, rateLimitErr := service.Login(context.Background(), "13800000001", testPassword)
	var limited *RateLimitedError
	if !errors.As(rateLimitErr, &limited) {
		t.Fatalf("error = %v, want RateLimitedError", rateLimitErr)
	}
	if limited.RetryAfter <= 0 || limited.RetryAfter > time.Minute {
		t.Fatalf("retryAfter = %s, want within the window", limited.RetryAfter)
	}
}

func TestIdentifyUnknownToken(t *testing.T) {
	service := newTestService(t, &fakeAccountReader{}, NewFixedWindowLimiter(10, time.Minute, nil))

	if _, err := service.Identify(context.Background(), "missing"); !errors.Is(err, ErrUnauthorized) {
		t.Fatalf("Identify(unknown) error = %v, want ErrUnauthorized", err)
	}
	if _, err := service.Identify(context.Background(), ""); !errors.Is(err, ErrUnauthorized) {
		t.Fatalf("Identify(empty) error = %v, want ErrUnauthorized", err)
	}
}

func TestLogoutIsIdempotent(t *testing.T) {
	reader := &fakeAccountReader{user: &UserAccount{
		ID: 1, Phone: "13800000001", PasswordHash: hashForTest(t, testPassword), Status: StatusActive,
	}}
	service := newTestService(t, reader, NewFixedWindowLimiter(10, time.Minute, nil))

	result, err := service.Login(context.Background(), "13800000001", testPassword)
	if err != nil {
		t.Fatalf("Login() error = %v", err)
	}
	if err := service.Logout(context.Background(), result.Token); err != nil {
		t.Fatalf("Logout() error = %v", err)
	}
	if _, err := service.Identify(context.Background(), result.Token); !errors.Is(err, ErrUnauthorized) {
		t.Fatalf("Identify() after logout error = %v", err)
	}
	if err := service.Logout(context.Background(), result.Token); err != nil {
		t.Fatalf("second Logout() error = %v, want idempotent no-op", err)
	}
}

func TestLoginTokensAreUnique(t *testing.T) {
	reader := &fakeAccountReader{user: &UserAccount{
		ID: 1, Phone: "13800000001", PasswordHash: hashForTest(t, testPassword), Status: StatusActive,
	}}
	service := newTestService(t, reader, NewFixedWindowLimiter(100, time.Minute, nil))

	first, err := service.Login(context.Background(), "13800000001", testPassword)
	if err != nil {
		t.Fatalf("first Login() error = %v", err)
	}
	second, err := service.Login(context.Background(), "13800000001", testPassword)
	if err != nil {
		t.Fatalf("second Login() error = %v", err)
	}
	if first.Token == second.Token {
		t.Fatal("two logins share a token")
	}
}

func TestLoginDatabaseFailureIsNotACredentialError(t *testing.T) {
	reader := &failingAccountReader{}
	codes := NewInMemorySMSCodeStore(nil)
	service, err := NewService(reader, reader, NewInMemoryAccountMutation(map[int64]*UserAccount{}), NewInMemorySessionStore(time.Minute, nil), NewFixedWindowLimiter(10, time.Minute, nil), codes, time.Minute, time.Hour)
	if err != nil {
		t.Fatalf("NewService() error = %v", err)
	}

	_, loginErr := service.Login(context.Background(), "13800000001", testPassword)
	if loginErr == nil || errors.Is(loginErr, ErrInvalidCredentials) {
		t.Fatalf("Login() error = %v, want a non-credential failure", loginErr)
	}
}

func TestNewServiceValidation(t *testing.T) {
	store := NewInMemorySessionStore(time.Minute, nil)
	limiter := NewFixedWindowLimiter(1, time.Minute, nil)
	codes := NewInMemorySMSCodeStore(nil)
	if _, err := NewService(nil, &fakeAccountReader{}, NewInMemoryAccountMutation(map[int64]*UserAccount{}), store, limiter, codes, time.Minute, time.Hour); err == nil {
		t.Fatal("nil account reader accepted")
	}
	if _, err := NewService(&fakeAccountReader{}, nil, NewInMemoryAccountMutation(map[int64]*UserAccount{}), store, limiter, codes, time.Minute, time.Hour); err == nil {
		t.Fatal("nil account writer accepted")
	}
	if _, err := NewService(&fakeAccountReader{}, &fakeAccountReader{}, nil, store, limiter, codes, time.Minute, time.Hour); err == nil {
		t.Fatal("nil session store accepted")
	}
	if _, err := NewService(&fakeAccountReader{}, &fakeAccountReader{}, NewInMemoryAccountMutation(map[int64]*UserAccount{}), store, nil, codes, time.Minute, time.Hour); err == nil {
		t.Fatal("nil limiter accepted")
	}
	if _, err := NewService(&fakeAccountReader{}, &fakeAccountReader{}, NewInMemoryAccountMutation(map[int64]*UserAccount{}), store, limiter, nil, time.Minute, time.Hour); err == nil {
		t.Fatal("nil sms code store accepted")
	}
	if _, err := NewService(&fakeAccountReader{}, &fakeAccountReader{}, NewInMemoryAccountMutation(map[int64]*UserAccount{}), store, limiter, codes, time.Hour, time.Minute); err == nil {
		t.Fatal("absolute TTL shorter than idle TTL accepted")
	}
}

func TestLoginBySmsRegistersUnknownPhone(t *testing.T) {
	reader := &fakeAccountReader{}
	codes := NewInMemorySMSCodeStore(nil)
	mutations := NewInMemoryAccountMutation(map[int64]*UserAccount{})
	if reader.user != nil {
		mutations.accounts[reader.user.ID] = reader.user
	}
	service, err := NewService(reader, reader, mutations, NewInMemorySessionStore(time.Minute, nil), NewFixedWindowLimiter(100, time.Minute, nil), codes, time.Minute, time.Hour)
	if err != nil {
		t.Fatalf("NewService() error = %v", err)
	}
	service.SMSMock = true
	ctx := context.Background()

	if _, err := service.IssueSMSCode(ctx, "13611112222"); err != nil {
		t.Fatalf("IssueSMSCode() error = %v", err)
	}
	stored, found := codes.Peek("13611112222")
	if err != nil || !found {
		t.Fatalf("peek code: found=%v err=%v", found, err)
	}

	// Wrong code is rejected (CODE_INVALID) and must not register the account.
	if _, err := service.LoginBySms(ctx, "13611112222", "000000"); !errors.Is(err, ErrSMSCodeInvalid) {
		t.Fatalf("wrong code error = %v, want ErrSMSCodeInvalid", err)
	}
	if reader.user != nil {
		t.Fatal("wrong code registered an account")
	}

	// The correct code registers the unknown phone with a wallet default.
	result, err := service.LoginBySms(ctx, "13611112222", stored)
	if err != nil {
		t.Fatalf("LoginBySms() error = %v", err)
	}
	if result.Identity.Role != RoleUser || result.Identity.ID != 99 || result.Identity.DisplayName != "用户2222" {
		t.Fatalf("registered identity = %#v", result.Identity)
	}

	// The code is consumed atomically: replaying it fails with CODE_INVALID.
	if _, err := service.LoginBySms(ctx, "13611112222", stored); !errors.Is(err, ErrSMSCodeInvalid) {
		t.Fatalf("replayed code error = %v, want ErrSMSCodeInvalid", err)
	}

	// Cooldown blocks immediate resend.
	if _, err := service.IssueSMSCode(ctx, "13611112222"); !errors.Is(err, ErrSMSCooldownActive) {
		t.Fatalf("cooldown error = %v, want ErrSMSCooldownActive", err)
	}
}

func TestLoginBySmsLocksOutAfterFiveFailures(t *testing.T) {
	reader := &fakeAccountReader{user: &UserAccount{ID: 5, Phone: "13611112222", Status: StatusActive}}
	codes := NewInMemorySMSCodeStore(nil)
	mutations := NewInMemoryAccountMutation(map[int64]*UserAccount{})
	if reader.user != nil {
		mutations.accounts[reader.user.ID] = reader.user
	}
	service, err := NewService(reader, reader, mutations, NewInMemorySessionStore(time.Minute, nil), NewFixedWindowLimiter(100, time.Minute, nil), codes, time.Minute, time.Hour)
	if err != nil {
		t.Fatalf("NewService() error = %v", err)
	}
	service.SMSMock = true
	ctx := context.Background()

	if _, err := service.IssueSMSCode(ctx, "13611112222"); err != nil {
		t.Fatalf("IssueSMSCode() error = %v", err)
	}
	stored, _ := codes.Peek("13611112222")

	for attempt := 1; attempt <= 5; attempt++ {
		_, err := service.LoginBySms(ctx, "13611112222", "000000")
		if !errors.Is(err, ErrSMSCodeInvalid) {
			t.Fatalf("failure %d: error = %v, want ErrSMSCodeInvalid", attempt, err)
		}
	}

	// The budget is exhausted: even the correct code is voided.
	if _, err := service.LoginBySms(ctx, "13611112222", stored); !errors.Is(err, ErrSMSCodeInvalid) {
		t.Fatalf("post-lockout error = %v, want ErrSMSCodeInvalid", err)
	}
}

func TestIssueSMSCodeRequiresMockOrProvider(t *testing.T) {
	reader := &fakeAccountReader{}
	service, err := NewService(reader, reader, NewInMemoryAccountMutation(map[int64]*UserAccount{}), NewInMemorySessionStore(time.Minute, nil), NewFixedWindowLimiter(100, time.Minute, nil), NewInMemorySMSCodeStore(nil), time.Minute, time.Hour)
	if err != nil {
		t.Fatalf("NewService() error = %v", err)
	}
	service.SMSMock = false

	_, err = service.IssueSMSCode(context.Background(), "13611112222")
	if !errors.Is(err, ErrSMSProviderNotConfigured) {
		t.Fatalf("error = %v, want ErrSMSProviderNotConfigured", err)
	}
}

func TestAdminRolePropagatesThroughSession(t *testing.T) {
	reader := &fakeAccountReader{admin: &AdminAccount{
		ID: 2, Username: "auditor", Role: AdminRoleAuditor, PasswordHash: hashForTest(t, testPassword), Status: StatusActive,
	}}
	service := newTestService(t, reader, NewFixedWindowLimiter(100, time.Minute, nil))

	result, err := service.LoginAdmin(context.Background(), "auditor", testPassword)
	if err != nil {
		t.Fatalf("LoginAdmin() error = %v", err)
	}
	if result.Identity.AdminRole != AdminRoleAuditor {
		t.Fatalf("admin role = %q, want AUDITOR", result.Identity.AdminRole)
	}
	if AdminCanWrite(result.Identity) {
		t.Fatal("auditor must not be allowed to write")
	}
	if !AdminCanRead(result.Identity) {
		t.Fatal("auditor must be allowed to read")
	}

	operator := &fakeAccountReader{admin: &AdminAccount{
		ID: 3, Username: "operator", Role: AdminRoleOperator, PasswordHash: hashForTest(t, testPassword), Status: StatusActive,
	}}
	operatorService := newTestService(t, operator, NewFixedWindowLimiter(100, time.Minute, nil))
	operatorResult, err := operatorService.LoginAdmin(context.Background(), "operator", testPassword)
	if err != nil {
		t.Fatalf("LoginAdmin(operator) error = %v", err)
	}
	if !AdminCanWrite(operatorResult.Identity) {
		t.Fatal("operator must be allowed to write")
	}
}
