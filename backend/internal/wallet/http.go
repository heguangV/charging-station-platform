package wallet

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"io"
	"net/http"

	"github.com/heguangV/charging-station-platform/backend/internal/auth"
	"github.com/heguangV/charging-station-platform/backend/internal/httpapi"
)

const (
	minIdempotencyKey = 16
	maxIdempotencyKey = 128
	maxJSONBodyBytes  = 16 << 10
)

// identityProvider supplies role-aware authentication; auth.Handlers implements it.
type identityProvider interface {
	RequireRole(role string, next http.HandlerFunc) http.HandlerFunc
	RequireAdminWrite(next http.HandlerFunc) http.HandlerFunc
}

// Handlers expose the wallet endpoints. All routes require a user session;
// the refund is admin-only and lives under /admin.
type Handlers struct {
	service *Service
	auth    identityProvider
}

// NewHandlers binds the wallet handlers to the service and auth middleware.
func NewHandlers(service *Service, auth identityProvider) (*Handlers, error) {
	if service == nil {
		return nil, errors.New("wallet: service is required")
	}
	if auth == nil {
		return nil, errors.New("wallet: auth middleware is required")
	}
	return &Handlers{service: service, auth: auth}, nil
}

// Register attaches every wallet route to the server.
func (h *Handlers) Register(server interface {
	Register(pattern string, handler http.HandlerFunc)
}) {
	server.Register("/api/v1/wallet", h.auth.RequireRole(auth.RoleUser, h.view))
	server.Register("/api/v1/wallet/top-up", h.auth.RequireRole(auth.RoleUser, h.topUp))
	server.Register("/api/v1/wallet/transactions", h.auth.RequireRole(auth.RoleUser, h.transactions))
	server.Register("/api/v1/admin/orders/{orderNo}/refund", h.auth.RequireAdminWrite(h.refundOrder))
}

func (h *Handlers) view(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodGet) {
		return
	}
	identity, ok := identityFrom(w, r)
	if !ok {
		return
	}

	result, err := h.service.View(r.Context(), identity.ID)
	if err != nil {
		writeWalletError(w, r, err)
		return
	}
	httpapi.WriteJSON(w, http.StatusOK, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    result,
	})
}

type topUpRequest struct {
	AmountCent int64 `json:"amountCent"`
}

// topUp handles POST /api/v1/wallet/top-up (UC-U-05 余额充值, simulated
// payment channel, idempotent through Idempotency-Key).
func (h *Handlers) topUp(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodPost) {
		return
	}
	identity, ok := identityFrom(w, r)
	if !ok {
		return
	}

	raw, err := io.ReadAll(http.MaxBytesReader(w, r.Body, maxJSONBodyBytes))
	if err != nil {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid request body", nil)
		return
	}
	var request topUpRequest
	if err := httpapi.DecodeJSONBytesStrict(raw, &request); err != nil {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid request body", nil)
		return
	}
	key, ok := requireIdempotencyKey(w, r)
	if !ok {
		return
	}
	if err := ValidateTopUpAmount(request.AmountCent); err != nil {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "top-up amount is outside 0.01..10000 yuan", nil)
		return
	}

	result, err := h.service.TopUp(r.Context(), TopUpCommand{
		UserID:         identity.ID,
		AmountCent:     request.AmountCent,
		IdempotencyKey: key,
		RequestHash:    requestHash(r, raw),
		TraceID:        httpapi.RequestID(r.Context()),
	})
	if err != nil {
		writeWalletError(w, r, err)
		return
	}
	httpapi.WriteJSON(w, http.StatusOK, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    result,
	})
}

func (h *Handlers) transactions(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodGet) {
		return
	}
	identity, ok := identityFrom(w, r)
	if !ok {
		return
	}

	page, pageSize, err := httpapi.ParsePagination(r)
	if err != nil {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid query parameter", nil)
		return
	}

	result, err := h.service.Transactions(r.Context(), EntryFilter{
		UserID:   identity.ID,
		Page:     page,
		PageSize: pageSize,
		Type:     r.URL.Query().Get("type"),
	})
	if err != nil {
		writeWalletError(w, r, err)
		return
	}
	httpapi.WriteJSON(w, http.StatusOK, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    result,
	})
}

// refundOrder handles POST /api/v1/admin/orders/{orderNo}/refund: returns a
// completed order's settled amount to the user's wallet (admin action, 200).
func (h *Handlers) refundOrder(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodPost) {
		return
	}
	identity, ok := identityFrom(w, r)
	if !ok {
		return
	}
	raw, err := io.ReadAll(http.MaxBytesReader(w, r.Body, maxJSONBodyBytes))
	if err != nil {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid request body", nil)
		return
	}
	var request struct {
		Reason string `json:"reason"`
	}
	if err := httpapi.DecodeJSONBytesStrict(raw, &request); err != nil {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid request body", nil)
		return
	}
	key, ok := requireIdempotencyKey(w, r)
	if !ok {
		return
	}

	result, err := h.service.RefundOrder(r.Context(), RefundCommand{
		AdminID:        identity.ID,
		OrderNo:        r.PathValue("orderNo"),
		IdempotencyKey: key,
		RequestHash:    requestHash(r, raw),
		TraceID:        httpapi.RequestID(r.Context()),
	})
	if err != nil {
		writeWalletError(w, r, err)
		return
	}
	httpapi.WriteJSON(w, http.StatusOK, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    result,
	})
}

func identityFrom(w http.ResponseWriter, r *http.Request) (auth.Identity, bool) {
	identity, ok := auth.IdentityFromContext(r.Context())
	if !ok {
		httpapi.WriteError(w, r, http.StatusUnauthorized, httpapi.CodeUnauthorized, "session is missing or expired", nil)
		return auth.Identity{}, false
	}
	return identity, true
}

func requireIdempotencyKey(w http.ResponseWriter, r *http.Request) (string, bool) {
	key := r.Header.Get("Idempotency-Key")
	if len(key) < minIdempotencyKey || len(key) > maxIdempotencyKey {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "Idempotency-Key header must be 16..128 characters", nil)
		return "", false
	}
	return key, true
}

func requestHash(r *http.Request, body []byte) string {
	hasher := sha256.New()
	hasher.Write([]byte(r.Method))
	hasher.Write([]byte(r.URL.Path))
	hasher.Write(body)
	return hex.EncodeToString(hasher.Sum(nil))
}

func requireMethod(w http.ResponseWriter, r *http.Request, method string) bool {
	if r.Method == method {
		return true
	}
	w.Header().Set("Allow", method)
	httpapi.WriteError(w, r, http.StatusMethodNotAllowed, httpapi.CodeMethodNotAllowed, "method not allowed", nil)
	return false
}

func writeWalletError(w http.ResponseWriter, r *http.Request, err error) {
	switch {
	case errors.Is(err, ErrInvalidTopUpAmount), errors.Is(err, ErrInvalidLedgerFilter):
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid request parameter", nil)
	case errors.Is(err, ErrIdempotencyConflict), errors.Is(err, ErrIdempotencyInProgress):
		httpapi.WriteError(w, r, http.StatusConflict, 14, "idempotency key conflict", nil)
	case errors.Is(err, ErrOrderNotRefundable):
		httpapi.WriteError(w, r, http.StatusConflict, 18, "order has no settled amount to refund", nil)
	case errors.Is(err, ErrOrderNotFound):
		httpapi.WriteError(w, r, http.StatusNotFound, httpapi.CodeResourceNotFound, "order not found", nil)
	case errors.Is(err, ErrWalletNotFound):
		httpapi.WriteError(w, r, http.StatusNotFound, 4, "wallet not found", nil)
	default:
		httpapi.WriteError(w, r, http.StatusServiceUnavailable, 3, "wallet is temporarily unavailable", nil)
	}
}
