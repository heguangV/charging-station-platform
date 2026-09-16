package order

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"strings"
	"time"

	"github.com/heguangV/charging-station-platform/backend/internal/auth"
	"github.com/heguangV/charging-station-platform/backend/internal/httpapi"
)

const (
	// minIdempotencyKey and maxIdempotencyKey mirror the contract
	// Idempotency-Key parameter bounds.
	minIdempotencyKey = 16
	maxIdempotencyKey = 128
	maxJSONBodyBytes  = 16 << 10
)

// identityProvider supplies RequireRole. auth.Handlers implements it, keeping
// the middleware behaviour identical across business modules.
type identityProvider interface {
	RequireRole(role string, next http.HandlerFunc) http.HandlerFunc
}

// Handlers expose the P0 order endpoints. All routes require a user session:
// admins work through the admin API (BE-A-05), and an admin identity has no
// user_accounts row to own orders.
type Handlers struct {
	service *Service
	auth    identityProvider
}

// NewHandlers binds the order handlers to the service and auth middleware.
func NewHandlers(service *Service, auth identityProvider) (*Handlers, error) {
	if service == nil {
		return nil, errors.New("order: service is required")
	}
	if auth == nil {
		return nil, errors.New("order: auth middleware is required")
	}
	return &Handlers{service: service, auth: auth}, nil
}

// Register attaches every order route to the server.
func (h *Handlers) Register(server interface {
	Register(pattern string, handler http.HandlerFunc)
}) {
	server.Register("/api/v1/orders", h.auth.RequireRole(auth.RoleUser, h.orders))
	server.Register("/api/v1/orders/{orderNo}", h.auth.RequireRole(auth.RoleUser, h.getOrder))
	server.Register("/api/v1/orders/{orderNo}/start", h.auth.RequireRole(auth.RoleUser, h.startCharging))
	server.Register("/api/v1/orders/{orderNo}/stop", h.auth.RequireRole(auth.RoleUser, h.stopCharging))
	server.Register("/api/v1/orders/{orderNo}/cancel", h.auth.RequireRole(auth.RoleUser, h.cancelOrder))
}

type createOrderRequest struct {
	ChargerID int64 `json:"chargerId"`
}

// orders handles POST /api/v1/orders (create) and GET /api/v1/orders (list).
func (h *Handlers) orders(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodPost:
		h.create(w, r)
	case http.MethodGet:
		h.list(w, r)
	default:
		w.Header().Set("Allow", "GET, POST")
		httpapi.WriteError(w, r, http.StatusMethodNotAllowed, httpapi.CodeMethodNotAllowed, "method not allowed", nil)
	}
}

func (h *Handlers) create(w http.ResponseWriter, r *http.Request) {
	identity, ok := identityFrom(w, r)
	if !ok {
		return
	}

	body, ok := readBody(w, r)
	if !ok {
		return
	}
	var request createOrderRequest
	if err := json.Unmarshal(body, &request); err != nil {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid request body", nil)
		return
	}

	key, ok := requireIdempotencyKey(w, r)
	if !ok {
		return
	}

	result, err := h.service.Create(r.Context(), CreateOrderCommand{
		UserID:         identity.ID,
		ChargerID:      request.ChargerID,
		IdempotencyKey: key,
		RequestHash:    requestHash(r, body),
		TraceID:        httpapi.RequestID(r.Context()),
	})
	if err != nil {
		writeOrderError(w, r, err)
		return
	}

	httpapi.WriteJSON(w, http.StatusCreated, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    result,
	})
}

func (h *Handlers) list(w http.ResponseWriter, r *http.Request) {
	identity, ok := identityFrom(w, r)
	if !ok {
		return
	}

	page, pageSize, err := httpapi.ParsePagination(r)
	if err != nil {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid query parameter", nil)
		return
	}
	createdFrom, ok := parseTimeQuery(w, r, "createdFrom")
	if !ok {
		return
	}
	createdTo, ok := parseTimeQuery(w, r, "createdTo")
	if !ok {
		return
	}

	result, err := h.service.List(r.Context(), ListFilter{
		UserID:      identity.ID,
		Page:        page,
		PageSize:    pageSize,
		Status:      r.URL.Query().Get("status"),
		CreatedFrom: createdFrom,
		CreatedTo:   createdTo,
		Sort:        r.URL.Query().Get("sort"),
	})
	if err != nil {
		writeOrderError(w, r, err)
		return
	}

	httpapi.WriteJSON(w, http.StatusOK, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data: map[string]any{
			"items": result.Items,
			"meta":  result.Meta,
		},
	})
}

func (h *Handlers) getOrder(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodGet) {
		return
	}
	identity, ok := identityFrom(w, r)
	if !ok {
		return
	}

	orderNo := r.PathValue("orderNo")
	result, err := h.service.Get(r.Context(), identity.ID, orderNo)
	if err != nil {
		writeOrderError(w, r, err)
		return
	}

	httpapi.WriteJSON(w, http.StatusOK, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    result,
	})
}

// startCharging handles POST /api/v1/orders/{orderNo}/start.
func (h *Handlers) startCharging(w http.ResponseWriter, r *http.Request) {
	h.transition(w, r, h.service.Start)
}

// stopCharging handles POST /api/v1/orders/{orderNo}/stop.
func (h *Handlers) stopCharging(w http.ResponseWriter, r *http.Request) {
	h.transition(w, r, h.service.Stop)
}

// cancelOrder handles POST /api/v1/orders/{orderNo}/cancel and releases the
// charger (UC-U-07: cancellable before charging starts, idempotent). The
// contract declares a synchronous 200, unlike the 202 start/stop commands.
func (h *Handlers) cancelOrder(w http.ResponseWriter, r *http.Request) {
	h.respondWithStatus(w, r, http.StatusOK, h.service.Cancel)
}

func (h *Handlers) transition(w http.ResponseWriter, r *http.Request, action func(ctx context.Context, command TransitionCommand) (Order, error)) {
	// 202 Accepted for start/stop: the request is recorded, the device
	// outcome arrives via the event loop, not in this response.
	h.respondWithStatus(w, r, http.StatusAccepted, action)
}

func (h *Handlers) respondWithStatus(w http.ResponseWriter, r *http.Request, successStatus int, action func(ctx context.Context, command TransitionCommand) (Order, error)) {
	if !requireMethod(w, r, http.MethodPost) {
		return
	}
	identity, ok := identityFrom(w, r)
	if !ok {
		return
	}

	key, ok := requireIdempotencyKey(w, r)
	if !ok {
		return
	}

	command := TransitionCommand{
		UserID:         identity.ID,
		OrderNo:        r.PathValue("orderNo"),
		IdempotencyKey: key,
		RequestHash:    requestHash(r, nil),
		TraceID:        httpapi.RequestID(r.Context()),
	}

	result, err := action(r.Context(), command)
	if err != nil {
		writeOrderError(w, r, err)
		return
	}

	httpapi.WriteJSON(w, successStatus, httpapi.Response{
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

// requireIdempotencyKey validates the contract Idempotency-Key header.
func requireIdempotencyKey(w http.ResponseWriter, r *http.Request) (string, bool) {
	key := r.Header.Get("Idempotency-Key")
	if len(key) < minIdempotencyKey || len(key) > maxIdempotencyKey {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "Idempotency-Key header must be 16..128 characters", nil)
		return "", false
	}
	return key, true
}

// requestHash fingerprints method, path and body so a reused key with a
// different request is rejected instead of replayed.
func requestHash(r *http.Request, body []byte) string {
	hasher := sha256.New()
	hasher.Write([]byte(r.Method))
	hasher.Write([]byte(r.URL.Path))
	hasher.Write(body)
	return hex.EncodeToString(hasher.Sum(nil))
}

func readBody(w http.ResponseWriter, r *http.Request) ([]byte, bool) {
	body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, maxJSONBodyBytes))
	if err != nil {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid request body", nil)
		return nil, false
	}
	return body, true
}

func requireMethod(w http.ResponseWriter, r *http.Request, method string) bool {
	if r.Method == method {
		return true
	}
	w.Header().Set("Allow", method)
	httpapi.WriteError(w, r, http.StatusMethodNotAllowed, httpapi.CodeMethodNotAllowed, "method not allowed", nil)
	return false
}

// writeOrderError maps domain errors to the shared error-code registry.
// parseTimeQuery reads an RFC3339 timestamp query parameter. Empty means "no
// bound". The registered parameter names for this endpoint are createdFrom and
// createdTo, so a value that does not parse is a 400 rather than a silently
// ignored filter - a user who typed a date and got the unfiltered list would
// have no way to notice.
func parseTimeQuery(w http.ResponseWriter, r *http.Request, name string) (*time.Time, bool) {
	raw := strings.TrimSpace(r.URL.Query().Get(name))
	if raw == "" {
		return nil, true
	}
	parsed, err := time.Parse(time.RFC3339, raw)
	if err != nil {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument,
			name+" must be an RFC3339 timestamp", nil)
		return nil, false
	}
	utc := parsed.UTC()
	return &utc, true
}

func writeOrderError(w http.ResponseWriter, r *http.Request, err error) {
	switch {
	case errors.Is(err, ErrOrderNotFound):
		httpapi.WriteError(w, r, http.StatusNotFound, httpapi.CodeResourceNotFound, "order not found", nil)
	case errors.Is(err, ErrInvalidOrderNo), errors.Is(err, ErrInvalidPagination),
		errors.Is(err, ErrInvalidStatusFilter), errors.Is(err, ErrInvalidChargerID),
		errors.Is(err, ErrInvalidPaymentStatus), errors.Is(err, ErrInvalidFactTime),
		errors.Is(err, ErrInvalidReceiptID), errors.Is(err, ErrInvalidSort),
		errors.Is(err, ErrInvalidTimeWindow):
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid request parameter", nil)
	case errors.Is(err, ErrUserFrozen):
		httpapi.WriteError(w, r, http.StatusForbidden, httpapi.CodeUserFrozen, "account is disabled", nil)
	case errors.Is(err, ErrDebtOutstanding):
		httpapi.WriteError(w, r, http.StatusConflict, codeDebtOutstanding, "user has an unsettled order", nil)
	case errors.Is(err, ErrInvalidStateTransition):
		httpapi.WriteError(w, r, http.StatusConflict, codeInvalidStateTransition, "invalid state transition", nil)
	case errors.Is(err, ErrChargerUnavailable):
		httpapi.WriteError(w, r, http.StatusConflict, codeChargerUnavailable, "charger is unavailable", nil)
	case errors.Is(err, ErrInsufficientBalance):
		httpapi.WriteError(w, r, http.StatusConflict, codeInsufficientBalance, "insufficient balance for the minimum start amount", nil)
	case errors.Is(err, ErrActiveFlowExists):
		httpapi.WriteError(w, r, http.StatusConflict, codeActiveFlowExists, "user already has an active flow", nil)
	case errors.Is(err, ErrIdempotencyConflict):
		httpapi.WriteError(w, r, http.StatusConflict, codeIdempotencyConflict, "idempotency key reused for a different request", nil)
	case errors.Is(err, ErrIdempotencyInProgress):
		httpapi.WriteError(w, r, http.StatusConflict, codeIdempotencyConflict, "identical request is still in progress", nil)
	case errors.Is(err, ErrFactTimeOutOfOrder):
		// An out-of-order receipt is a device fact the state machine cannot
		// accept, not a malformed request: the charger reports a stop before a
		// start, or repeats a confirmation the order has already moved past.
		httpapi.WriteError(w, r, http.StatusConflict, codeInvalidStateTransition, "device receipt is out of order", nil)
	case errors.Is(err, ErrChargerOrderMismatch):
		httpapi.WriteError(w, r, http.StatusConflict, codeInvalidStateTransition, "receipt does not match the order's charger", nil)
	default:
		httpapi.WriteError(w, r, http.StatusServiceUnavailable, httpapi.CodeDatabaseError, "order processing is temporarily unavailable", nil)
	}
}

// Registry error codes not yet needed by earlier modules (docs/database-api.md
// §1.10, append-only).
const (
	codeInsufficientBalance    = 7  // INSUFFICIENT_BALANCE
	codeChargerUnavailable     = 8  // CHARGER_UNAVAILABLE
	codeActiveFlowExists       = 9  // ACTIVE_FLOW_EXISTS
	codeIdempotencyConflict    = 14 // IDEMPOTENCY_CONFLICT
	codeInvalidStateTransition = 15 // INVALID_STATE_TRANSITION
	codeDebtOutstanding        = 18 // DEBT_OUTSTANDING
)

// contextT is an alias so handler helpers can share signatures without
// repeating the import list in closures.
type contextT = context.Context
