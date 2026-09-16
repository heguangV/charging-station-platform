package auth

import (
	"context"
	"errors"
	"math"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/heguangV/charging-station-platform/backend/internal/httpapi"
)

// Registry codes for SMS verification outcomes (docs/database-api.md §1.10).
const (
	codeSMSCodeInvalid      = 20 // CODE_INVALID
	codeAlreadyExists       = 5  // ALREADY_EXISTS
	codeExternalServiceDown = 12 // EXTERNAL_SERVICE_UNAVAILABLE
)

type contextKey string

const identityContextKey contextKey = "auth_identity"

// Handlers expose the auth endpoints and the authentication middleware.
// Method enforcement stays here so every error keeps the unified envelope.
type Handlers struct {
	service *Service
}

// NewHandlers binds the handlers to the auth service.
func NewHandlers(service *Service) (*Handlers, error) {
	if service == nil {
		return nil, errors.New("auth: service is required")
	}
	return &Handlers{service: service}, nil
}

// identityBody is the contract Identity payload. adminRole is present only
// for administrators.
type identityBody struct {
	ID          int64  `json:"id"`
	Role        string `json:"role"`
	AdminRole   string `json:"adminRole,omitempty"`
	DisplayName string `json:"displayName"`
	Status      string `json:"status"`
}

type loginRequest struct {
	Account  string `json:"account"`
	Password string `json:"password"`
}

type loginResponse struct {
	AccessToken string       `json:"accessToken"`
	ExpiresAt   string       `json:"expiresAt"`
	Identity    identityBody `json:"identity"`
}

type smsCodeRequest struct {
	Phone string `json:"phone"`
}

type smsLoginRequest struct {
	Phone string `json:"phone"`
	Code  string `json:"code"`
}

type smsCodeResponse struct {
	Phone        string `json:"phone"`
	ExpiresInSec int64  `json:"expiresInSec"`
	// Code is only returned when simulated SMS delivery is configured
	// (development environments, SRS UC-U-01).
	Code string `json:"code,omitempty"`
}

type smsLoginResponse struct {
	AccessToken string       `json:"accessToken"`
	ExpiresAt   string       `json:"expiresAt"`
	Identity    identityBody `json:"identity"`
}

// Register attaches every auth route to the server.
func (h *Handlers) Register(server interface {
	Register(pattern string, handler http.HandlerFunc)
}) {
	server.Register("/api/v1/auth/user/sms/code", h.RequestSMSCode)
	server.Register("/api/v1/auth/user/login/sms", h.SMSLogin)
	server.Register("/api/v1/auth/user/login", h.UserLogin)
	server.Register("/api/v1/auth/user/register", h.RegisterUser)
	server.Register("/api/v1/auth/admin/login", h.AdminLogin)
	server.Register("/api/v1/auth/logout", h.Logout)
	server.Register("/api/v1/me", h.RequireIdentity(h.meRoutes))
	server.Register("/api/v1/me/profile", h.RequireRole(RoleUser, h.Profile))
	server.Register("/api/v1/admin/users/{userId}/freeze", h.RequireAdminWrite(h.freezeUser))
	server.Register("/api/v1/admin/users/{userId}/unfreeze", h.RequireAdminWrite(h.unfreezeUser))
}

// RequestSMSCode handles POST /api/v1/auth/user/sms/code. Development
// environments return the generated code in the response (simulated SMS);
// without simulated delivery the request fails 503 because no provider is
// wired yet.
func (h *Handlers) RequestSMSCode(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodPost) {
		return
	}

	body, ok := decodeJSONBody(w, r, &smsCodeRequest{})
	if !ok {
		return
	}
	request := body.(*smsCodeRequest)

	code, err := h.service.IssueSMSCode(r.Context(), request.Phone)
	if err != nil {
		writeAuthError(w, r, err)
		return
	}

	response := smsCodeResponse{
		Phone:        strings.TrimSpace(request.Phone),
		ExpiresInSec: int64(smsCodeTTL.Seconds()),
	}
	if h.service.SMSMock {
		response.Code = code
	}
	httpapi.WriteJSON(w, http.StatusOK, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    response,
	})
}

// SMSLogin handles POST /api/v1/auth/user/login/sms. Unknown phones register
// automatically with a wallet (UC-U-01).
func (h *Handlers) SMSLogin(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodPost) {
		return
	}

	body, ok := decodeJSONBody(w, r, &smsLoginRequest{})
	if !ok {
		return
	}
	request := body.(*smsLoginRequest)

	result, err := h.service.LoginBySms(r.Context(), request.Phone, request.Code)
	if err != nil {
		writeAuthError(w, r, err)
		return
	}

	httpapi.WriteJSON(w, http.StatusOK, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    newLoginResponse(result),
	})
}

// UserLogin handles POST /api/v1/auth/user/login.
// registerUserRequest is the registration body. username is optional: the SMS
// login path names an account from its phone, and registration keeps that shape
// when the caller does not supply a name.
type registerUserRequest struct {
	Username string `json:"username"`
	Phone    string `json:"phone"`
	Password string `json:"password"`
	SmsCode  string `json:"smsCode"`
	DeviceID string `json:"deviceId"`
}

// RegisterUser creates an account and returns a session (201). The account is
// created only after the SMS code proves the caller owns the phone, and a phone
// that already has an account is 409 rather than taken over.
func (h *Handlers) RegisterUser(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodPost) {
		return
	}

	body, ok := decodeJSONBody(w, r, &registerUserRequest{})
	if !ok {
		return
	}
	request := body.(*registerUserRequest)

	result, err := h.service.Register(r.Context(), request.Username, request.Phone, request.Password, request.SmsCode)
	if err != nil {
		writeAuthError(w, r, err)
		return
	}

	httpapi.WriteJSON(w, http.StatusCreated, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    newLoginResponse(result),
	})
}

func (h *Handlers) UserLogin(w http.ResponseWriter, r *http.Request) {
	h.login(w, r, h.service.Login)
}

// AdminLogin handles POST /api/v1/auth/admin/login.
func (h *Handlers) AdminLogin(w http.ResponseWriter, r *http.Request) {
	h.login(w, r, h.service.LoginAdmin)
}

func (h *Handlers) login(w http.ResponseWriter, r *http.Request, action func(ctx context.Context, account, password string) (LoginResult, error)) {
	if !requireMethod(w, r, http.MethodPost) {
		return
	}

	body, ok := decodeJSONBody(w, r, &loginRequest{})
	if !ok {
		return
	}
	request := body.(*loginRequest)

	// Field-length violations are malformed requests (400), not failed
	// authentication; the service guard behind this stays as defense.
	if len(request.Account) < minAccountLength || len(request.Account) > maxAccountLength ||
		len(request.Password) < minPasswordLength || len(request.Password) > maxPasswordLength {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "account or password length out of range", nil)
		return
	}

	result, err := action(r.Context(), request.Account, request.Password)
	if err != nil {
		writeAuthError(w, r, err)
		return
	}

	httpapi.WriteJSON(w, http.StatusOK, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    newLoginResponse(result),
	})
}

// Logout handles POST /api/v1/auth/logout. Revoking an unknown token still
// returns 204 so logout is idempotent.
func (h *Handlers) Logout(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodPost) {
		return
	}

	token, ok := bearerToken(w, r)
	if !ok {
		return
	}
	if err := h.service.Logout(r.Context(), token); err != nil {
		httpapi.WriteError(w, r, http.StatusServiceUnavailable, httpapi.CodeDatabaseError, "logout failed", nil)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

// meRoutes dispatches GET /api/v1/me (identity) and PUT/DELETE on the same
// resource namespace.
func (h *Handlers) meRoutes(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		h.Me(w, r)
	case http.MethodPut:
		if !requireContextRole(w, r, RoleUser) {
			return
		}
		h.updateProfile(w, r)
	case http.MethodDelete:
		if !requireContextRole(w, r, RoleUser) {
			return
		}
		h.deleteAccount(w, r)
	default:
		w.Header().Set("Allow", "GET, PUT, DELETE")
		httpapi.WriteError(w, r, http.StatusMethodNotAllowed, httpapi.CodeMethodNotAllowed, "method not allowed", nil)
	}
}

func requireContextRole(w http.ResponseWriter, r *http.Request, role string) bool {
	identity, ok := IdentityFromContext(r.Context())
	if !ok || !Authorize(identity, role) {
		httpapi.WriteError(w, r, http.StatusForbidden, httpapi.CodeForbidden, "insufficient permission", nil)
		return false
	}
	return true
}

// Profile returns the user-center profile view (UC-U-05) on GET
// /api/v1/me/profile and applies changes on PUT.
func (h *Handlers) Profile(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		h.getProfile(w, r)
	case http.MethodPut:
		h.updateProfile(w, r)
	default:
		w.Header().Set("Allow", "GET, PUT")
		httpapi.WriteError(w, r, http.StatusMethodNotAllowed, httpapi.CodeMethodNotAllowed, "method not allowed", nil)
	}
}

type updateProfileRequest struct {
	DisplayName *string `json:"displayName"`
	AvatarURL   *string `json:"avatarUrl"`
}

func (h *Handlers) getProfile(w http.ResponseWriter, r *http.Request) {
	identity, ok := identityFrom(w, r)
	if !ok {
		return
	}
	view, err := h.service.GetProfile(r.Context(), identity.ID)
	if err != nil {
		writeProfileError(w, r, err)
		return
	}
	httpapi.WriteJSON(w, http.StatusOK, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    view,
	})
}

func (h *Handlers) updateProfile(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodPut) {
		return
	}
	identity, ok := identityFrom(w, r)
	if !ok {
		return
	}

	body, ok := decodeJSONBody(w, r, &updateProfileRequest{})
	if !ok {
		return
	}
	request := body.(*updateProfileRequest)
	if request.DisplayName == nil && request.AvatarURL == nil {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "no profile change requested", nil)
		return
	}

	view, err := h.service.UpdateProfile(r.Context(), identity.ID, ProfileUpdate{
		DisplayName: request.DisplayName,
		AvatarURL:   request.AvatarURL,
	})
	if err != nil {
		writeProfileError(w, r, err)
		return
	}
	httpapi.WriteJSON(w, http.StatusOK, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    view,
	})
}

// deleteAccount handles DELETE /api/v1/me (UC-U-05 申请注销): the account is
// anonymized and every session of the user is revoked, including the
// current one — the response is 204 with no further content.
func (h *Handlers) deleteAccount(w http.ResponseWriter, r *http.Request) {
	identity, ok := identityFrom(w, r)
	if !ok {
		return
	}
	token, _ := parseBearerToken(r.Header.Get("Authorization"))
	existed, err := h.service.DeleteAccount(r.Context(), identity.ID, token)
	if err != nil {
		writeProfileError(w, r, err)
		return
	}
	_ = existed
	w.WriteHeader(http.StatusNoContent)
}

// freezeUser handles POST /api/v1/admin/users/{userId}/freeze.
func (h *Handlers) freezeUser(w http.ResponseWriter, r *http.Request) {
	h.setFrozen(w, r, true)
}

// unfreezeUser handles POST /api/v1/admin/users/{userId}/unfreeze.
func (h *Handlers) unfreezeUser(w http.ResponseWriter, r *http.Request) {
	h.setFrozen(w, r, false)
}

func (h *Handlers) setFrozen(w http.ResponseWriter, r *http.Request, frozen bool) {
	if !requireMethod(w, r, http.MethodPost) {
		return
	}
	identity, ok := identityFrom(w, r)
	if !ok {
		return
	}
	userID, err := strconv.ParseInt(r.PathValue("userId"), 10, 64)
	if err != nil || userID < 1 {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid user id", nil)
		return
	}
	var operationErr error
	if frozen {
		operationErr = h.service.FreezeUser(r.Context(), identity.ID, userID)
	} else {
		operationErr = h.service.UnfreezeUser(r.Context(), identity.ID, userID)
	}
	if operationErr != nil {
		writeProfileError(w, r, operationErr)
		return
	}
	httpapi.WriteJSON(w, http.StatusOK, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    map[string]any{"id": userID, "status": map[bool]string{true: "DISABLED", false: "ACTIVE"}[frozen]},
	})
}

func writeProfileError(w http.ResponseWriter, r *http.Request, err error) {
	switch {
	case errors.Is(err, ErrInvalidNickname):
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "nickname length is outside 1..20 or pure whitespace", nil)
	case errors.Is(err, ErrProfileNotFound), errors.Is(err, ErrAccountDeleted):
		httpapi.WriteError(w, r, http.StatusNotFound, httpapi.CodeResourceNotFound, "account not found", nil)
	case errors.Is(err, ErrInvalidAdminActor):
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "administrator actor is required", nil)
	default:
		httpapi.WriteError(w, r, http.StatusServiceUnavailable, httpapi.CodeDatabaseError, "profile is temporarily unavailable", nil)
	}
}

// Me handles GET /api/v1/me.
func (h *Handlers) Me(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodGet) {
		return
	}

	identity, ok := h.requireIdentity(w, r)
	if !ok {
		return
	}

	httpapi.WriteJSON(w, http.StatusOK, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    newIdentityBody(identity),
	})
}

// RequireIdentity wraps a handler so it only runs with a valid bearer token.
// The resolved identity is attached to the request context.
func (h *Handlers) RequireIdentity(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		identity, ok := h.requireIdentity(w, r)
		if !ok {
			return
		}
		next(w, r.WithContext(WithIdentity(r.Context(), identity)))
	}
}

// RequireRole additionally enforces a role (for example RoleAdmin) and maps
// mismatches to 403 FORBIDDEN.
func (h *Handlers) RequireRole(role string, next http.HandlerFunc) http.HandlerFunc {
	return h.RequireIdentity(func(w http.ResponseWriter, r *http.Request) {
		identity, _ := IdentityFromContext(r.Context())
		if !Authorize(identity, role) {
			httpapi.WriteError(w, r, http.StatusForbidden, httpapi.CodeForbidden, "insufficient permission", nil)
			return
		}
		next(w, r)
	})
}

// RequireAdminWrite additionally enforces the write-capable admin roles
// (SUPER_ADMIN, OPERATOR): read-only auditors receive 403 FORBIDDEN.
func (h *Handlers) RequireAdminWrite(next http.HandlerFunc) http.HandlerFunc {
	return h.RequireRole(RoleAdmin, func(w http.ResponseWriter, r *http.Request) {
		identity, _ := IdentityFromContext(r.Context())
		if !AdminCanWrite(identity) {
			httpapi.WriteError(w, r, http.StatusForbidden, httpapi.CodeForbidden, "insufficient permission for administrative writes", nil)
			return
		}
		next(w, r)
	})
}

func (h *Handlers) requireIdentity(w http.ResponseWriter, r *http.Request) (Identity, bool) {
	token, ok := bearerToken(w, r)
	if !ok {
		return Identity{}, false
	}

	identity, err := h.service.Identify(r.Context(), token)
	if err != nil {
		switch {
		case errors.Is(err, ErrUnauthorized):
			httpapi.WriteError(w, r, http.StatusUnauthorized, httpapi.CodeUnauthorized, "session is missing or expired", nil)
			return Identity{}, false
		case errors.Is(err, ErrUserFrozen):
			httpapi.WriteError(w, r, http.StatusForbidden, httpapi.CodeUserFrozen, "account is disabled", nil)
			return Identity{}, false
		}
		httpapi.WriteError(w, r, http.StatusServiceUnavailable, httpapi.CodeDatabaseError, "session check failed", nil)
		return Identity{}, false
	}
	return identity, true
}

func bearerToken(w http.ResponseWriter, r *http.Request) (string, bool) {
	token, ok := parseBearerToken(r.Header.Get("Authorization"))
	if !ok {
		httpapi.WriteError(w, r, http.StatusUnauthorized, httpapi.CodeUnauthorized, "bearer token is required", nil)
		return "", false
	}
	return token, true
}

func parseBearerToken(header string) (string, bool) {
	parts := strings.Fields(header)
	if len(parts) != 2 || !strings.EqualFold(parts[0], "Bearer") || parts[1] == "" {
		return "", false
	}
	return parts[1], true
}

func writeAuthError(w http.ResponseWriter, r *http.Request, err error) {
	switch {
	case errors.Is(err, ErrInvalidPhone), errors.Is(err, ErrInvalidSMSCode):
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid phone number or code", nil)
	case errors.Is(err, ErrSMSCooldownActive):
		httpapi.WriteError(w, r, http.StatusTooManyRequests, httpapi.CodeRateLimited, "sms code was requested recently", map[string]any{"retryAfterSec": int64(smsResendCooldown.Seconds())})
	case errors.Is(err, ErrSMSCodeInvalid):
		httpapi.WriteError(w, r, http.StatusUnprocessableEntity, codeSMSCodeInvalid, "sms code is wrong, expired, or exceeded its failure budget", nil)
	case errors.Is(err, ErrSMSProviderNotConfigured):
		httpapi.WriteError(w, r, http.StatusServiceUnavailable, codeExternalServiceDown, "no sms provider is configured", nil)
	case errors.Is(err, ErrInvalidCredentials):
		httpapi.WriteError(w, r, http.StatusUnauthorized, httpapi.CodeUnauthorized, "invalid credentials", nil)
	case errors.Is(err, ErrAccountExists):
		httpapi.WriteError(w, r, http.StatusConflict, codeAlreadyExists, "phone is already registered", nil)
	case errors.Is(err, ErrPasswordLength):
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "password must be 8..128 characters", nil)
	case errors.Is(err, ErrUserFrozen):
		httpapi.WriteError(w, r, http.StatusForbidden, httpapi.CodeUserFrozen, "account is disabled", nil)
	case errors.Is(err, ErrUnauthorized):
		httpapi.WriteError(w, r, http.StatusUnauthorized, httpapi.CodeUnauthorized, "session is missing or expired", nil)
	case isRateLimited(err):
		var limited *RateLimitedError
		errors.As(err, &limited)
		retryAfterSec := int64(math.Ceil(limited.RetryAfter.Seconds()))
		if retryAfterSec < 1 {
			retryAfterSec = 1
		}
		httpapi.WriteError(w, r, http.StatusTooManyRequests, httpapi.CodeRateLimited, "too many requests", map[string]any{"retryAfterSec": retryAfterSec})
	default:
		// Credential lookup or session persistence failed; the request never
		// reached the caller's fault. Database details stay in server logs.
		httpapi.WriteError(w, r, http.StatusServiceUnavailable, httpapi.CodeDatabaseError, "login is temporarily unavailable", nil)
	}
}

func isRateLimited(err error) bool {
	var limited *RateLimitedError
	return errors.As(err, &limited)
}

// identityFrom resolves the middleware-attached identity or writes 401.
func identityFrom(w http.ResponseWriter, r *http.Request) (Identity, bool) {
	identity, ok := IdentityFromContext(r.Context())
	if !ok {
		httpapi.WriteError(w, r, http.StatusUnauthorized, httpapi.CodeUnauthorized, "session is missing or expired", nil)
		return Identity{}, false
	}
	return identity, true
}

func newIdentityBody(identity Identity) identityBody {
	return identityBody{
		ID:          identity.ID,
		Role:        identity.Role,
		AdminRole:   identity.AdminRole,
		DisplayName: identity.DisplayName,
		Status:      identity.Status,
	}
}

func newLoginResponse(result LoginResult) loginResponse {
	return loginResponse{
		AccessToken: result.Token,
		ExpiresAt:   result.ExpiresAt.UTC().Format(time.RFC3339),
		Identity:    newIdentityBody(result.Identity),
	}
}

func requireMethod(w http.ResponseWriter, r *http.Request, method string) bool {
	if r.Method == method {
		return true
	}
	w.Header().Set("Allow", method)
	httpapi.WriteError(w, r, http.StatusMethodNotAllowed, httpapi.CodeMethodNotAllowed, "method not allowed", nil)
	return false
}

const maxJSONBodyBytes = 64 << 10

// decodeJSONBody parses a bounded JSON body. On failure the error envelope is
// already written and ok is false.
func decodeJSONBody(w http.ResponseWriter, r *http.Request, target any) (any, bool) {
	reader := http.MaxBytesReader(w, r.Body, maxJSONBodyBytes)
	if err := httpapi.DecodeJSONStrict(reader, target); err != nil {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid request body", nil)
		return nil, false
	}
	return target, true
}

// IdentityFromContext returns the identity attached by RequireIdentity.
func IdentityFromContext(ctx context.Context) (Identity, bool) {
	identity, ok := ctx.Value(identityContextKey).(Identity)
	return identity, ok
}

// WithIdentity attaches an identity to a context. It is part of the
// middleware contract and lets composed middleware and tests supply the
// identity the same way RequireIdentity does.
func WithIdentity(ctx context.Context, identity Identity) context.Context {
	return context.WithValue(ctx, identityContextKey, identity)
}
