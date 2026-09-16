package auth

import (
	"context"
	"crypto/rand"
	"encoding/base64"
	"errors"
	"fmt"
	"strings"
	"time"
)

// Role and status values shared with the frozen contract. AdminRole carries
// the finer-grained administrator roles from admin_accounts so the admin API
// can enforce read-only auditors instead of a flat ADMIN.
const (
	RoleUser      = "USER"
	RoleAdmin     = "ADMIN"
	StatusActive  = "ACTIVE"
	StatusDisable = "DISABLED"

	AdminRoleSuper    = "SUPER_ADMIN"
	AdminRoleOperator = "OPERATOR"
	AdminRoleAuditor  = "AUDITOR"
)

const (
	minAccountLength = 3
	maxAccountLength = 128
	tokenByteLength  = 32
)

// Identity is the contract Identity payload: the authenticated caller.
type Identity struct {
	ID          int64
	Role        string
	AdminRole   string // SUPER_ADMIN/OPERATOR/AUDITOR for admins, empty for users
	DisplayName string
	Status      string
}

// UserAccount is the persisted user row the service needs. PasswordHash uses
// the package hash format; it never leaves the service.
type UserAccount struct {
	ID           int64
	Phone        string
	Email        string
	DisplayName  string
	PasswordHash string
	Status       string
}

// AdminAccount is the persisted admin row the service needs.
type AdminAccount struct {
	ID           int64
	Username     string
	Role         string // SUPER_ADMIN, OPERATOR or AUDITOR
	PasswordHash string
	Status       string
}

// AccountReader is the PostgreSQL boundary for credential lookup.
// Implementations return (nil, nil) when no account matches.
type AccountReader interface {
	FindUserByAccount(ctx context.Context, account string) (*UserAccount, error)
	FindAdminByUsername(ctx context.Context, username string) (*AdminAccount, error)
}

// AccountWriter ensures an account exists. UC-U-01 requires the first SMS
// login to register the user and the wallet; the method executes the same
// statement sequence whether or not the account already exists, so response
// timing cannot be used to enumerate registered phones.
type AccountWriter interface {
	EnsureUserWithWallet(ctx context.Context, phone string) (UserAccount, error)
	// RegisterUser creates a user with a password and its wallet. It reports
	// ErrAccountExists when the phone is already registered instead of taking
	// the account over: the SMS login path upserts deliberately, but a
	// registration that silently claimed an existing phone would let anyone
	// take over an account by typing someone else's number.
	RegisterUser(ctx context.Context, phone, displayName, passwordHash string) (UserAccount, error)
}

// Sentinel errors mapped by the HTTP layer to the shared error-code registry.
var (
	// ErrInvalidCredentials maps to 401 UNAUTHORIZED. It is deliberately
	// shared by unknown account and wrong password so neither is distinguishable.
	ErrInvalidCredentials = errors.New("auth: invalid credentials")
	// ErrAccountExists maps to 409 ALREADY_EXISTS: the phone already has an
	// account, and registration does not take it over.
	ErrAccountExists = errors.New("auth: phone is already registered")
	// ErrUserFrozen maps to 403 USER_FROZEN.
	ErrUserFrozen = errors.New("auth: account is disabled")
	// ErrUnauthorized maps to 401 UNAUTHORIZED for missing or expired sessions.
	ErrUnauthorized = errors.New("auth: session is missing or expired")
	// ErrSMSCooldownActive maps to 429 RATE_LIMITED: codes cannot be resent
	// before the resend window ends.
	ErrSMSCooldownActive = errors.New("auth: sms code cooldown is active")
	// ErrInvalidPhone and ErrInvalidSMSCode map to 400 INVALID_ARGUMENT.
	ErrInvalidPhone   = errors.New("auth: invalid phone number")
	ErrInvalidSMSCode = errors.New("auth: invalid sms code")
	// ErrSMSCodeInvalid maps to 422 CODE_INVALID: wrong code, expired code,
	// or the failure budget is exhausted.
	ErrSMSCodeInvalid = errors.New("auth: sms code is wrong, expired, or exceeded its failure budget")
	// ErrSMSProviderNotConfigured maps to 503 EXTERNAL_SERVICE_UNAVAILABLE:
	// simulated delivery is off and no SMS provider is wired yet.
	ErrSMSProviderNotConfigured = errors.New("auth: no sms provider is configured")
)

// RateLimitedError maps to 429 RATE_LIMITED and carries the retry hint.
type RateLimitedError struct {
	RetryAfter time.Duration
}

func (e *RateLimitedError) Error() string {
	return fmt.Sprintf("auth: rate limited, retry after %s", e.RetryAfter)
}

// LoginResult is a successful login: the opaque token, its absolute expiry
// and the identity it represents.
type LoginResult struct {
	Token     string
	ExpiresAt time.Time
	Identity  Identity
}

// Service implements login (password and SMS), session validation, logout
// and role checks.
type Service struct {
	accounts    AccountReader
	writer      AccountWriter
	mutations   AccountMutation
	sessions    SessionStore
	limiter     LoginRateLimiter
	codes       SMSCodeStore
	idleTTL     time.Duration
	absoluteTTL time.Duration
	clock       func() time.Time

	// SMSMock enables simulated SMS delivery: the generated code is returned
	// in the response so development environments can complete the login.
	// Formal environments must disable it (AGENTS: 禁用模拟短信) and wire a
	// real provider.
	SMSMock bool

	// Sender delivers codes when simulation is off. A formal deployment
	// without a sender fails code requests with 503 until one is wired.
	Sender SMSSender
}

// NewService wires the dependencies. The idle window must not exceed the
// absolute lifetime. The account writer and SMS code store back the SMS
// login path; SMS codes live in Redis so verification works across
// instances and restarts.
func NewService(accounts AccountReader, writer AccountWriter, mutations AccountMutation, sessions SessionStore, limiter LoginRateLimiter, codes SMSCodeStore, idleTTL, absoluteTTL time.Duration) (*Service, error) {
	if accounts == nil {
		return nil, errors.New("auth: account reader is required")
	}
	if writer == nil {
		return nil, errors.New("auth: account writer is required")
	}
	if mutations == nil {
		return nil, errors.New("auth: account mutation store is required")
	}
	if sessions == nil {
		return nil, errors.New("auth: session store is required")
	}
	if limiter == nil {
		return nil, errors.New("auth: login rate limiter is required")
	}
	if codes == nil {
		return nil, errors.New("auth: sms code store is required")
	}
	if idleTTL <= 0 || absoluteTTL <= 0 {
		return nil, errors.New("auth: session TTLs must be positive")
	}
	if absoluteTTL < idleTTL {
		return nil, errors.New("auth: absolute TTL must not be shorter than idle TTL")
	}

	return &Service{
		accounts:    accounts,
		writer:      writer,
		mutations:   mutations,
		sessions:    sessions,
		limiter:     limiter,
		codes:       codes,
		idleTTL:     idleTTL,
		absoluteTTL: absoluteTTL,
		clock:       time.Now,
	}, nil
}

// Login authenticates a user by phone or email and creates a session.
func (s *Service) Login(ctx context.Context, account, password string) (LoginResult, error) {
	return s.login(ctx, "user", account, password, s.loginUser)
}

// LoginAdmin authenticates an administrator by username and creates a session.
func (s *Service) LoginAdmin(ctx context.Context, username, password string) (LoginResult, error) {
	return s.login(ctx, "admin", username, password, s.loginAdmin)
}

type accountLookup func(ctx context.Context, account string) (*userOrAdmin, error)

type userOrAdmin struct {
	id           int64
	displayName  string
	role         string // admin role, empty for users
	passwordHash string
	status       string
}

func (s *Service) login(ctx context.Context, kind, account, password string, lookup accountLookup) (LoginResult, error) {
	account = strings.TrimSpace(account)
	if len(account) < minAccountLength || len(account) > maxAccountLength || len(password) < minPasswordLength || len(password) > maxPasswordLength {
		return LoginResult{}, ErrInvalidCredentials
	}

	// The limiter runs before any database read so brute force cannot reach
	// PostgreSQL, and the counter advances on failures as well as successes.
	if allowed, retryAfter := s.limiter.Allow(ctx, kind+":"+strings.ToLower(account)); !allowed {
		return LoginResult{}, &RateLimitedError{RetryAfter: retryAfter}
	}

	found, err := lookup(ctx, account)
	if err != nil {
		return LoginResult{}, err
	}

	// Verify against a dummy hash when the account is unknown so the response
	// timing of unknown accounts matches the wrong-password path.
	stored := DummyHash()
	if found != nil {
		stored = found.passwordHash
	}
	if !VerifyPassword(password, stored) {
		return LoginResult{}, ErrInvalidCredentials
	}
	if found == nil {
		return LoginResult{}, ErrInvalidCredentials
	}
	if found.status != StatusActive {
		return LoginResult{}, ErrUserFrozen
	}

	return s.createSession(ctx, found.id, roleFor(kind), found.role, found.displayName)
}

// createSession mints the token, persists the session and assembles the
// identity shared by every login path.
func (s *Service) createSession(ctx context.Context, identityID int64, role, adminRole, displayName string) (LoginResult, error) {
	token, err := newToken()
	if err != nil {
		return LoginResult{}, fmt.Errorf("auth: generate session token: %w", err)
	}

	now := s.clock()
	session := Session{
		IdentityID:  identityID,
		Role:        role,
		AdminRole:   adminRole,
		DisplayName: displayName,
		Status:      StatusActive,
		ExpiresAt:   now.Add(s.absoluteTTL),
	}
	if err := s.sessions.Save(ctx, token, session); err != nil {
		return LoginResult{}, fmt.Errorf("auth: save session: %w", err)
	}

	return LoginResult{
		Token:     token,
		ExpiresAt: session.ExpiresAt,
		Identity: Identity{
			ID:          identityID,
			Role:        role,
			AdminRole:   adminRole,
			DisplayName: displayName,
			Status:      StatusActive,
		},
	}, nil
}

// Register creates a user account from a phone, a password and a valid SMS code
// and returns a session, so the caller is logged in without a second round trip.
//
// The order of the checks is deliberate: phone and password are validated first
// (the caller's own input), the code is verified next (the proof that the phone
// belongs to the caller), and only then is the account created. Creating the
// account before the code check would leave a half-registered account behind on
// every typo, and verifying the code before validating the body would let anyone
// burn another phone's code with a malformed request.
func (s *Service) Register(ctx context.Context, username, phone, password, code string) (LoginResult, error) {
	phone = strings.TrimSpace(phone)
	username = strings.TrimSpace(username)
	if !IsValidPhone(phone) {
		return LoginResult{}, ErrInvalidPhone
	}
	if !IsValidSMSCode(code) {
		return LoginResult{}, ErrInvalidSMSCode
	}
	// HashPassword enforces the contract's 8..128 bounds, so a weak password is
	// rejected before anything is stored.
	passwordHash, err := HashPassword(password)
	if err != nil {
		return LoginResult{}, err
	}

	verified, _, found, err := s.codes.Verify(ctx, phone, code, maxSMSCodeFailures)
	if err != nil {
		return LoginResult{}, fmt.Errorf("auth: verify sms code: %w", err)
	}
	if !found || !verified {
		return LoginResult{}, ErrSMSCodeInvalid
	}

	if username == "" {
		// The same default the SMS login path uses, so a user who registers and
		// one who logs in by SMS end up with the same display-name shape.
		username = "用户" + phone[len(phone)-4:]
	}

	user, err := s.writer.RegisterUser(ctx, phone, username, passwordHash)
	if err != nil {
		return LoginResult{}, err
	}
	return s.createSession(ctx, user.ID, RoleUser, "", user.DisplayName)
}

// LoginBySms authenticates a user by phone and SMS code. Unknown phones are
// registered automatically together with a zero-balance wallet (UC-U-01).
func (s *Service) LoginBySms(ctx context.Context, phone, code string) (LoginResult, error) {
	phone = strings.TrimSpace(phone)
	if !IsValidPhone(phone) {
		return LoginResult{}, ErrInvalidPhone
	}
	if !IsValidSMSCode(code) {
		return LoginResult{}, ErrInvalidSMSCode
	}

	if allowed, retryAfter := s.limiter.Allow(ctx, "sms:"+phone); !allowed {
		return LoginResult{}, &RateLimitedError{RetryAfter: retryAfter}
	}

	verified, lockedOut, found, err := s.codes.Verify(ctx, phone, code, maxSMSCodeFailures)
	if err != nil {
		return LoginResult{}, fmt.Errorf("auth: verify sms code: %w", err)
	}
	if !found || !verified {
		// Wrong code, expired code, or the failure budget is exhausted —
		// all map to CODE_INVALID without distinguishing them for attackers.
		_ = lockedOut
		return LoginResult{}, ErrSMSCodeInvalid
	}

	// Both paths run the same statement sequence (INSERT ON CONFLICT +
	// SELECT), so a registered phone cannot be enumerated through timing.
	user, err := s.writer.EnsureUserWithWallet(ctx, phone)
	if err != nil {
		return LoginResult{}, fmt.Errorf("auth: ensure user account: %w", err)
	}
	if user.Status != StatusActive {
		return LoginResult{}, ErrUserFrozen
	}

	return s.createSession(ctx, user.ID, RoleUser, "", user.DisplayName)
}

// IssueSMSCode generates and stores a fresh login code for the phone. With
// simulated delivery (development only) the code is returned so the caller
// can complete the login; without simulation the request fails until an SMS
// provider is wired — storing a code nobody delivers would return success
// while the user never receives anything.
func (s *Service) IssueSMSCode(ctx context.Context, phone string) (string, error) {
	phone = strings.TrimSpace(phone)
	if !IsValidPhone(phone) {
		return "", ErrInvalidPhone
	}
	if !s.SMSMock && s.Sender == nil {
		return "", ErrSMSProviderNotConfigured
	}

	allowed, err := s.codes.BeginCooldown(ctx, phone, smsResendCooldown)
	if err != nil {
		return "", fmt.Errorf("auth: sms cooldown: %w", err)
	}
	if !allowed {
		return "", ErrSMSCooldownActive
	}

	code, err := NewSMSCode()
	if err != nil {
		return "", err
	}
	if err := s.codes.Issue(ctx, phone, code, smsCodeTTL); err != nil {
		return "", fmt.Errorf("auth: store sms code: %w", err)
	}

	if !s.SMSMock {
		// A delivery failure must not strand the user inside the resend
		// cooldown: release it so the request can be retried at once.
		if err := s.Sender.SendLoginCode(ctx, phone, code); err != nil {
			_ = s.codes.ClearCooldown(ctx, phone)
			return "", fmt.Errorf("auth: deliver sms code: %w", err)
		}
	}
	return code, nil
}

func (s *Service) loginUser(ctx context.Context, account string) (*userOrAdmin, error) {
	user, err := s.accounts.FindUserByAccount(ctx, account)
	if err != nil {
		return nil, fmt.Errorf("auth: find user account: %w", err)
	}
	if user == nil {
		return nil, nil
	}
	return &userOrAdmin{
		id:           user.ID,
		displayName:  user.DisplayName,
		passwordHash: user.PasswordHash,
		status:       user.Status,
	}, nil
}

func (s *Service) loginAdmin(ctx context.Context, username string) (*userOrAdmin, error) {
	admin, err := s.accounts.FindAdminByUsername(ctx, username)
	if err != nil {
		return nil, fmt.Errorf("auth: find admin account: %w", err)
	}
	if admin == nil {
		return nil, nil
	}
	return &userOrAdmin{
		id:           admin.ID,
		displayName:  admin.Username,
		role:         admin.Role,
		passwordHash: admin.PasswordHash,
		status:       admin.Status,
	}, nil
}

// Identify resolves a bearer token to its identity or ErrUnauthorized.
func (s *Service) Identify(ctx context.Context, token string) (Identity, error) {
	token = strings.TrimSpace(token)
	if token == "" {
		return Identity{}, ErrUnauthorized
	}

	session, err := s.sessions.Load(ctx, token)
	if err != nil {
		if errors.Is(err, ErrSessionNotFound) {
			return Identity{}, ErrUnauthorized
		}
		return Identity{}, fmt.Errorf("auth: load session: %w", err)
	}

	return Identity{
		ID:          session.IdentityID,
		Role:        session.Role,
		AdminRole:   session.AdminRole,
		DisplayName: session.DisplayName,
		Status:      session.Status,
	}, nil
}

// Logout revokes the token. Revoking an unknown token is a no-op so logout
// stays idempotent.
func (s *Service) Logout(ctx context.Context, token string) error {
	if err := s.sessions.Delete(ctx, strings.TrimSpace(token)); err != nil {
		return fmt.Errorf("auth: delete session: %w", err)
	}
	return nil
}

// Authorize reports whether the identity satisfies the required role.
// An empty required role means any authenticated identity is accepted.
func Authorize(identity Identity, requiredRole string) bool {
	if identity.Status != StatusActive {
		return false
	}
	if requiredRole == "" {
		return true
	}
	return identity.Role == requiredRole
}

// AdminCanWrite reports whether an admin identity may perform mutating
// administration. SUPER_ADMIN and OPERATOR may write; AUDITOR is read-only
// (SRS admin module). It always rejects user identities.
func AdminCanWrite(identity Identity) bool {
	if !Authorize(identity, RoleAdmin) {
		return false
	}
	return identity.AdminRole == AdminRoleSuper || identity.AdminRole == AdminRoleOperator
}

// AdminCanRead reports whether an admin identity may read administrative
// data: every admin role, auditors included.
func AdminCanRead(identity Identity) bool {
	if !Authorize(identity, RoleAdmin) {
		return false
	}
	switch identity.AdminRole {
	case AdminRoleSuper, AdminRoleOperator, AdminRoleAuditor:
		return true
	default:
		return false
	}
}

func roleFor(kind string) string {
	if kind == "admin" {
		return RoleAdmin
	}
	return RoleUser
}

func newToken() (string, error) {
	buffer := make([]byte, tokenByteLength)
	if _, err := rand.Read(buffer); err != nil {
		return "", err
	}
	return base64.RawURLEncoding.EncodeToString(buffer), nil
}
