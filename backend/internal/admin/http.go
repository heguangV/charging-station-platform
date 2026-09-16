package admin

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"strconv"
	"strings"

	"github.com/heguangV/charging-station-platform/backend/internal/auth"
	"github.com/heguangV/charging-station-platform/backend/internal/httpapi"
	"github.com/heguangV/charging-station-platform/backend/internal/order"
	"github.com/heguangV/charging-station-platform/backend/internal/station"
)

const (
	minIdempotencyKey = 16
	maxIdempotencyKey = 128
	maxJSONBodyBytes  = 16 << 10
)

// identityProvider supplies the admin middleware; auth.Handlers implements it.
type identityProvider interface {
	RequireRole(role string, next http.HandlerFunc) http.HandlerFunc
	RequireAdminWrite(next http.HandlerFunc) http.HandlerFunc
}

// Handlers expose the management endpoints. Reads accept every admin role,
// writes (station creation, device commands) require SUPER_ADMIN or
// OPERATOR.
type Handlers struct {
	service *Service
	auth    identityProvider
}

// NewHandlers binds the admin handlers to the service and auth middleware.
func NewHandlers(service *Service, auth identityProvider) (*Handlers, error) {
	if service == nil {
		return nil, errors.New("admin: service is required")
	}
	if auth == nil {
		return nil, errors.New("admin: auth middleware is required")
	}
	return &Handlers{service: service, auth: auth}, nil
}

// Register attaches every admin route to the server.
func (h *Handlers) Register(server interface {
	Register(pattern string, handler http.HandlerFunc)
}) {
	server.Register("/api/v1/admin/stations", h.auth.RequireRole(auth.RoleAdmin, h.stations))
	server.Register("/api/v1/admin/chargers", h.auth.RequireRole(auth.RoleAdmin, h.listChargers))
	server.Register("/api/v1/admin/chargers/{chargerId}/restart", h.auth.RequireAdminWrite(h.restartCharger))
	// Reading a command's outcome is a read: any administrator role, including
	// AUDITOR, may look it up; starting the command is the write.
	server.Register("/api/v1/admin/device-commands/{commandId}", h.auth.RequireRole(auth.RoleAdmin, h.getDeviceCommand))
	// Status changes are writes: RequireAdminWrite keeps the read-only AUDITOR
	// role (and any non-admin) out, which answers with 403.
	server.Register("/api/v1/admin/stations/{stationId}/status", h.auth.RequireAdminWrite(h.changeStationStatus))
	server.Register("/api/v1/admin/chargers/{chargerId}/status", h.auth.RequireAdminWrite(h.changeChargerStatus))
	server.Register("/api/v1/admin/users", h.auth.RequireRole(auth.RoleAdmin, h.users))
	server.Register("/api/v1/admin/orders", h.auth.RequireRole(auth.RoleAdmin, h.listOrders))
	server.Register("/api/v1/admin/users/{userId}", h.auth.RequireRole(auth.RoleAdmin, h.userDetail))
	server.Register("/api/v1/admin/users/{userId}/transactions", h.auth.RequireRole(auth.RoleAdmin, h.userLedger))
	h.registerStats(server)
	h.registerProfile(server)
	h.registerChargerBatch(server)
	h.registerUserArchive(server)
	server.Register("/api/v1/admin/accounts", h.auth.RequireRole(auth.RoleAdmin, h.adminAccounts))
	server.Register("/api/v1/admin/accounts/{accountId}/status", h.auth.RequireRole(auth.RoleAdmin, h.adminAccountStatus))
	server.Register("/api/v1/admin/chargers/{chargerId}/tariff", h.tariffRoutes)
	server.Register("/api/v1/admin/chargers/{chargerId}/release", h.auth.RequireAdminWrite(h.forceRelease))
	server.Register("/api/v1/admin/audit", h.auth.RequireRole(auth.RoleAdmin, h.listAudit))
}

// identityFrom is enforced by the middleware; every admin handler needs the
// acting administrator for audit trails and idempotency scopes.
// tariffRoutes dispatches GET and PUT on /admin/chargers/{chargerId}/tariff.
// GET is readable by every admin role (SRS: 审计员可读); PUT requires the
// write-capable roles — the two policies are applied per branch because one
// route pattern serves both methods.
func (h *Handlers) tariffRoutes(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		h.auth.RequireRole(auth.RoleAdmin, h.getTariff)(w, r)
	case http.MethodPut:
		h.auth.RequireAdminWrite(h.updateTariff)(w, r)
	default:
		w.Header().Set("Allow", "GET, PUT")
		httpapi.WriteError(w, r, http.StatusMethodNotAllowed, httpapi.CodeMethodNotAllowed, "method not allowed", nil)
	}
}

type updateTariffRequest struct {
	ElectricityPriceCent int64  `json:"electricityPriceCentPerKwh"`
	ServicePriceCent     int64  `json:"servicePriceCentPerKwh"`
	OffPeakPriceCent     *int64 `json:"offPeakElectricityPriceCentPerKwh"`
	OffPeakStartHour     *int16 `json:"offPeakStartHour"`
	OffPeakEndHour       *int16 `json:"offPeakEndHour"`
}

func (h *Handlers) getTariff(w http.ResponseWriter, r *http.Request) {
	chargerID, ok := chargerIDFrom(w, r)
	if !ok {
		return
	}
	result, err := h.service.GetTariff(r.Context(), chargerID)
	if err != nil {
		writeAdminError(w, r, err)
		return
	}
	httpapi.WriteJSON(w, http.StatusOK, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    result,
	})
}

func (h *Handlers) updateTariff(w http.ResponseWriter, r *http.Request) {
	identity, ok := identityFrom(w, r)
	if !ok {
		return
	}
	chargerID, ok := chargerIDFrom(w, r)
	if !ok {
		return
	}
	body, ok := readBody(w, r)
	if !ok {
		return
	}
	var request updateTariffRequest
	if err := httpapi.DecodeJSONBytesStrict(body, &request); err != nil {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid request body", nil)
		return
	}

	result, err := h.service.UpdateTariff(r.Context(), TariffUpdate{
		AdminID:              identity.ID,
		ChargerID:            chargerID,
		ElectricityPriceCent: request.ElectricityPriceCent,
		ServicePriceCent:     request.ServicePriceCent,
		OffPeakPriceCent:     request.OffPeakPriceCent,
		OffPeakStartHour:     request.OffPeakStartHour,
		OffPeakEndHour:       request.OffPeakEndHour,
	})
	if err != nil {
		writeAdminError(w, r, err)
		return
	}
	httpapi.WriteJSON(w, http.StatusOK, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    result,
	})
}

type forceReleaseRequest struct {
	Reason       string `json:"reason"`
	TargetStatus string `json:"targetStatus"`
}

// forceRelease handles POST /api/v1/admin/chargers/{chargerId}/release
// (BR-11 forced release with reason, target status and audit).
func (h *Handlers) forceRelease(w http.ResponseWriter, r *http.Request) {
	identity, ok := identityFrom(w, r)
	if !ok {
		return
	}
	chargerID, ok := chargerIDFrom(w, r)
	if !ok {
		return
	}
	body, ok := readBody(w, r)
	if !ok {
		return
	}
	var request forceReleaseRequest
	if err := httpapi.DecodeJSONBytesStrict(body, &request); err != nil {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid request body", nil)
		return
	}
	key, ok := requireIdempotencyKey(w, r)
	if !ok {
		return
	}

	result, err := h.service.ForceRelease(r.Context(), ForceReleaseCommand{
		AdminID:        identity.ID,
		ChargerID:      chargerID,
		Reason:         request.Reason,
		TargetStatus:   request.TargetStatus,
		IdempotencyKey: key,
		RequestHash:    requestHash(r, body),
		TraceID:        httpapi.RequestID(r.Context()),
	})
	if err != nil {
		writeAdminError(w, r, err)
		return
	}
	httpapi.WriteJSON(w, http.StatusOK, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    result,
	})
}

// userDetail handles GET /api/v1/admin/users/{userId}.
func (h *Handlers) userDetail(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodGet) {
		return
	}
	userID, err := strconv.ParseInt(r.PathValue("userId"), 10, 64)
	if err != nil || userID < 1 {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid user id", nil)
		return
	}
	result, err := h.service.UserDetail(r.Context(), userID)
	if err != nil {
		writeAdminError(w, r, err)
		return
	}
	httpapi.WriteJSON(w, http.StatusOK, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    result,
	})
}

// userLedger handles GET /api/v1/admin/users/{userId}/transactions.
func (h *Handlers) userLedger(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodGet) {
		return
	}
	userID, err := strconv.ParseInt(r.PathValue("userId"), 10, 64)
	if err != nil || userID < 1 {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid user id", nil)
		return
	}
	page, pageSize, ok := h.parsePagination(w, r)
	if !ok {
		return
	}

	result, err := h.service.UserLedger(r.Context(), UserLedgerFilter{
		UserID: userID, Page: page, PageSize: pageSize, Type: r.URL.Query().Get("type"),
	})
	if err != nil {
		writeAdminError(w, r, err)
		return
	}
	writePage(w, r, http.StatusOK, result)
}

// listAudit handles GET /api/v1/admin/audit.
func (h *Handlers) listAudit(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodGet) {
		return
	}
	page, pageSize, ok := h.parsePagination(w, r)
	if !ok {
		return
	}
	query := r.URL.Query()

	result, err := h.service.Audit(r.Context(), AuditFilter{
		Page:         page,
		PageSize:     pageSize,
		ActorID:      query.Get("actorId"),
		Action:       query.Get("action"),
		ResourceType: query.Get("resourceType"),
		ResourceID:   query.Get("resourceId"),
	})
	if err != nil {
		writeAdminError(w, r, err)
		return
	}
	writePage(w, r, http.StatusOK, result)
}

func chargerIDFrom(w http.ResponseWriter, r *http.Request) (int64, bool) {
	chargerID, err := strconv.ParseInt(r.PathValue("chargerId"), 10, 64)
	if err != nil || chargerID < 1 {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid charger id", nil)
		return 0, false
	}
	return chargerID, true
}

func identityFrom(w http.ResponseWriter, r *http.Request) (auth.Identity, bool) {
	identity, ok := auth.IdentityFromContext(r.Context())
	if !ok {
		httpapi.WriteError(w, r, http.StatusUnauthorized, httpapi.CodeUnauthorized, "session is missing or expired", nil)
		return auth.Identity{}, false
	}
	return identity, true
}

type createStationRequest struct {
	Code        string `json:"code"`
	Name        string `json:"name"`
	Address     string `json:"address"`
	LatitudeE6  int64  `json:"latitudeE6"`
	LongitudeE6 int64  `json:"longitudeE6"`
}

// stations handles GET /api/v1/admin/stations (all statuses) and
// POST /api/v1/admin/stations (create, SUPER_ADMIN/OPERATOR only).
func (h *Handlers) stations(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		h.listStations(w, r)
	case http.MethodPost:
		h.createStation(w, r)
	default:
		w.Header().Set("Allow", "GET, POST")
		httpapi.WriteError(w, r, http.StatusMethodNotAllowed, httpapi.CodeMethodNotAllowed, "method not allowed", nil)
	}
}

func (h *Handlers) listStations(w http.ResponseWriter, r *http.Request) {
	page, pageSize, ok := h.parsePagination(w, r)
	if !ok {
		return
	}
	keyword := r.URL.Query().Get("keyword")
	if len(keyword) > 100 {
		writeInvalidQuery(w, r)
		return
	}

	result, err := h.service.store.ListStations(r.Context(), page, pageSize, keyword)
	if err != nil {
		writeAdminError(w, r, err)
		return
	}
	writePage(w, r, http.StatusOK, result)
}

func (h *Handlers) createStation(w http.ResponseWriter, r *http.Request) {
	identity, ok := identityFrom(w, r)
	if !ok {
		return
	}
	// GET and POST share one route pattern, so the write check runs in the
	// handler: read-only auditors must not create stations.
	if !auth.AdminCanWrite(identity) {
		httpapi.WriteError(w, r, http.StatusForbidden, httpapi.CodeForbidden, "insufficient permission for administrative writes", nil)
		return
	}
	body, ok := readBody(w, r)
	if !ok {
		return
	}
	var request createStationRequest
	if err := httpapi.DecodeJSONBytesStrict(body, &request); err != nil {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid request body", nil)
		return
	}
	key, ok := requireIdempotencyKey(w, r)
	if !ok {
		return
	}

	result, err := h.service.Create(r.Context(), CreateStationCommand{
		AdminID:        identity.ID,
		Code:           request.Code,
		Name:           request.Name,
		Address:        request.Address,
		LatitudeE6:     request.LatitudeE6,
		LongitudeE6:    request.LongitudeE6,
		IdempotencyKey: key,
		RequestHash:    requestHash(r, body),
		TraceID:        httpapi.RequestID(r.Context()),
	})
	if err != nil {
		writeAdminError(w, r, err)
		return
	}

	stationBody := map[string]any{
		"id":                 result.ID,
		"code":               result.Code,
		"name":               result.Name,
		"address":            result.Address,
		"status":             result.Status,
		"latitudeE6":         result.LatitudeE6,
		"longitudeE6":        result.LongitudeE6,
		"chargerCount":       0,
		"idleChargerCount":   0,
		"minPriceCentPerKwh": 0,
	}
	httpapi.WriteJSON(w, http.StatusCreated, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    stationBody,
	})
}

func (h *Handlers) listChargers(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodGet) {
		return
	}
	page, pageSize, ok := h.parsePagination(w, r)
	if !ok {
		return
	}
	query := r.URL.Query()
	filter := chargerFilter{Page: page, PageSize: pageSize}
	if raw := query.Get("stationId"); raw != "" {
		value, err := strconv.ParseInt(raw, 10, 64)
		if err != nil || value < 1 {
			httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid station id", nil)
			return
		}
		filter.StationID = value
		filter.StationIDSet = true
	}
	if raw := query.Get("status"); raw != "" {
		filter.Status = raw
	}

	result, err := h.service.store.ListChargers(r.Context(), filter)
	if err != nil {
		writeAdminError(w, r, err)
		return
	}
	writePage(w, r, http.StatusOK, result)
}

func (h *Handlers) listUsers(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodGet) {
		return
	}
	page, pageSize, ok := h.parsePagination(w, r)
	if !ok {
		return
	}
	query := r.URL.Query()
	filter := UserFilter{Page: page, PageSize: pageSize, Keyword: query.Get("keyword")}
	if len(filter.Keyword) > 100 {
		writeInvalidQuery(w, r)
		return
	}
	if raw := query.Get("status"); raw != "" {
		// The contract encodes the user status as an integer: 1 active, 0 disabled.
		value, err := strconv.Atoi(raw)
		if err != nil || value < 0 || value > 1 {
			httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid user status", nil)
			return
		}
		filter.Status = UserStatusActive
		if value == UserStatusQueryDisabled {
			filter.Status = UserStatusDisabled
		}
	}

	result, err := h.service.store.ListUsers(r.Context(), filter)
	if err != nil {
		writeAdminError(w, r, err)
		return
	}
	writePage(w, r, http.StatusOK, result)
}

func (h *Handlers) listOrders(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodGet) {
		return
	}
	page, pageSize, ok := h.parsePagination(w, r)
	if !ok {
		return
	}
	query := r.URL.Query()
	filter := AdminOrderFilter{Page: page, PageSize: pageSize, OrderNo: query.Get("orderNo"), Status: query.Get("status")}
	if len(filter.OrderNo) > 64 {
		writeInvalidQuery(w, r)
		return
	}
	if raw := strings.TrimSpace(query.Get("userId")); raw != "" {
		userID, err := strconv.ParseInt(raw, 10, 64)
		if err != nil || userID < 1 {
			writeInvalidQuery(w, r)
			return
		}
		filter.UserID = userID
	}
	result, err := h.service.store.ListOrders(r.Context(), filter)
	if err != nil {
		writeAdminError(w, r, err)
		return
	}
	writePage(w, r, http.StatusOK, result)
}

func (h *Handlers) getDeviceCommand(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodGet) {
		return
	}
	commandID := strings.TrimSpace(r.PathValue("commandId"))
	result, err := h.service.DeviceCommand(r.Context(), commandID)
	if err != nil {
		writeAdminError(w, r, err)
		return
	}
	httpapi.WriteJSON(w, http.StatusOK, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    result,
	})
}

// statusRequest is the body of both status endpoints.
type statusRequest struct {
	Status string `json:"status"`
}

// changeStationStatus takes a station out of service or brings it back. The
// transition table lives in the station domain, and an illegal transition is a
// 409 rather than a silent write.
func (h *Handlers) changeStationStatus(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodPut) {
		return
	}
	identity, ok := identityFrom(w, r)
	if !ok {
		return
	}
	stationID, ok := pathID(w, r, "stationId", "station id")
	if !ok {
		return
	}
	status, ok := readStatusBody(w, r)
	if !ok {
		return
	}

	result, err := h.service.ChangeStationStatus(r.Context(), ChangeStationStatusCommand{
		AdminID:   identity.ID,
		StationID: stationID,
		Status:    status,
		TraceID:   httpapi.RequestID(r.Context()),
	})
	if err != nil {
		writeAdminError(w, r, err)
		return
	}
	httpapi.WriteJSON(w, http.StatusOK, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    result,
	})
}

// changeChargerStatus is the same operation for one charger.
func (h *Handlers) changeChargerStatus(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodPut) {
		return
	}
	identity, ok := identityFrom(w, r)
	if !ok {
		return
	}
	chargerID, ok := pathID(w, r, "chargerId", "charger id")
	if !ok {
		return
	}
	status, ok := readStatusBody(w, r)
	if !ok {
		return
	}

	result, err := h.service.ChangeChargerStatus(r.Context(), ChangeChargerStatusCommand{
		AdminID:   identity.ID,
		ChargerID: chargerID,
		Status:    status,
		TraceID:   httpapi.RequestID(r.Context()),
	})
	if err != nil {
		writeAdminError(w, r, err)
		return
	}
	httpapi.WriteJSON(w, http.StatusOK, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    result,
	})
}

// pathID reads a positive integer path parameter, answering 400 itself when the
// value is missing or unusable.
func pathID(w http.ResponseWriter, r *http.Request, name, label string) (int64, bool) {
	value, err := strconv.ParseInt(r.PathValue(name), 10, 64)
	if err != nil || value < 1 {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid "+label, nil)
		return 0, false
	}
	return value, true
}

// readStatusBody reads {"status": "..."} from the request body. An empty status
// is rejected here so the domain never has to guess what "" meant.
func readStatusBody(w http.ResponseWriter, r *http.Request) (string, bool) {
	body, ok := readBody(w, r)
	if !ok {
		return "", false
	}
	var request statusRequest
	if err := json.Unmarshal(body, &request); err != nil {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid request body", nil)
		return "", false
	}
	status := strings.TrimSpace(request.Status)
	if status == "" {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "status is required", nil)
		return "", false
	}
	return status, true
}

func (h *Handlers) restartCharger(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodPost) {
		return
	}
	identity, ok := identityFrom(w, r)
	if !ok {
		return
	}

	body, ok := readBody(w, r)
	if !ok {
		return
	}
	var request struct {
		Reason string `json:"reason"`
	}
	if err := httpapi.DecodeJSONBytesStrict(body, &request); err != nil {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid request body", nil)
		return
	}
	key, ok := requireIdempotencyKey(w, r)
	if !ok {
		return
	}
	chargerID, err := strconv.ParseInt(r.PathValue("chargerId"), 10, 64)
	if err != nil || chargerID < 1 {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid charger id", nil)
		return
	}

	result, err := h.service.Restart(r.Context(), RestartCommand{
		AdminID:        identity.ID,
		ChargerID:      chargerID,
		Reason:         request.Reason,
		IdempotencyKey: key,
		RequestHash:    requestHash(r, body),
		TraceID:        httpapi.RequestID(r.Context()),
	})
	if err != nil {
		writeAdminError(w, r, err)
		return
	}

	// 202 Accepted: the command is queued, the device outcome arrives via
	// the event loop.
	httpapi.WriteJSON(w, http.StatusAccepted, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    result,
	})
}

// shared helpers -----------------------------------------------------------

type chargerFilter = station.ChargerFilter

func (h *Handlers) parsePagination(w http.ResponseWriter, r *http.Request) (page, pageSize int64, ok bool) {
	page, pageSize, err := httpapi.ParsePagination(r)
	if err != nil {
		writeInvalidQuery(w, r)
		return 0, 0, false
	}
	return page, pageSize, true
}

func writePage(w http.ResponseWriter, r *http.Request, status int, result any) {
	httpapi.WriteJSON(w, status, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    result,
	})
}

func writeAdminError(w http.ResponseWriter, r *http.Request, err error) {
	switch {
	case errors.Is(err, ErrUserNotFound):
		httpapi.WriteError(w, r, http.StatusNotFound, httpapi.CodeResourceNotFound, "user not found", nil)
	case errors.Is(err, ErrDeviceCommandNotFound):
		httpapi.WriteError(w, r, http.StatusNotFound, httpapi.CodeResourceNotFound, "device command not found", nil)
	case errors.Is(err, order.ErrIdempotencyConflict), errors.Is(err, order.ErrIdempotencyInProgress):
		// The contract requires 409 for a reused idempotency key.
		httpapi.WriteError(w, r, http.StatusConflict, codeIdempotencyConflict, "idempotency key conflict", nil)
	case errors.Is(err, ErrInvalidChargerBatch):
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, err.Error(), nil)
	case errors.Is(err, ErrDuplicateChargerCode):
		// The request is well formed; it collides with what exists. The registry's
		// ALREADY_EXISTS is exactly this, and 409 tells the caller to change the
		// code rather than to retry.
		httpapi.WriteError(w, r, http.StatusConflict, codeAlreadyExists, err.Error(), nil)
	case errors.Is(err, ErrInvalidUserDraft), errors.Is(err, ErrInvalidUserBatch):
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, err.Error(), nil)
	case errors.Is(err, ErrDuplicateUserPhone):
		// A phone is the account's identity, so a collision is the same kind of
		// answer as a duplicate charger code: 409 ALREADY_EXISTS, naming the
		// problem rather than hinting at a retry.
		httpapi.WriteError(w, r, http.StatusConflict, codeAlreadyExists, err.Error(), nil)
	case errors.Is(err, ErrChargerUnavailable):
		httpapi.WriteError(w, r, http.StatusConflict, codeChargerUnavailable, "charger is unavailable", nil)
	case errors.Is(err, ErrInvalidStateTransition):
		httpapi.WriteError(w, r, http.StatusConflict, codeInvalidStateTransition, "invalid state transition", nil)
	case errors.Is(err, ErrStationNotFound):
		httpapi.WriteError(w, r, http.StatusNotFound, httpapi.CodeResourceNotFound, "station not found", nil)
	case errors.Is(err, ErrChargerNotFound):
		httpapi.WriteError(w, r, http.StatusNotFound, httpapi.CodeResourceNotFound, "charger not found", nil)
	case errors.Is(err, ErrAdminAccountNotFound):
		httpapi.WriteError(w, r, http.StatusNotFound, httpapi.CodeResourceNotFound, "administrator account not found", nil)
	case errors.Is(err, ErrAdminAccountExists):
		httpapi.WriteError(w, r, http.StatusConflict, codeAlreadyExists, "administrator username already exists", nil)
	case errors.Is(err, ErrAdminAccountVersionConflict):
		httpapi.WriteError(w, r, http.StatusConflict, 22, "administrator account version conflict", nil)
	case errors.Is(err, ErrAdminAccountSelfDisable), errors.Is(err, ErrLastSuperAdmin):
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, err.Error(), nil)
	case errors.Is(err, ErrInvalidStationFilter), errors.Is(err, ErrInvalidStationProfile),
		errors.Is(err, ErrInvalidUserStatus), errors.Is(err, ErrInvalidReason),
		errors.Is(err, ErrInvalidTariff), errors.Is(err, ErrInvalidLedgerFilter),
		errors.Is(err, ErrInvalidStatusValue), errors.Is(err, ErrInvalidAdminAccount):
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid request parameter", nil)
	default:
		httpapi.WriteError(w, r, http.StatusServiceUnavailable, httpapi.CodeDatabaseError, "administration is temporarily unavailable", nil)
	}
}

// Registry codes (docs/database-api.md §1.10).
const (
	codeAlreadyExists          = 5  // ALREADY_EXISTS, 409
	codeChargerUnavailable     = 8  // CHARGER_UNAVAILABLE
	codeIdempotencyConflict    = 14 // IDEMPOTENCY_CONFLICT
	codeInvalidStateTransition = 15 // INVALID_STATE_TRANSITION
)

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

func writeInvalidQuery(w http.ResponseWriter, r *http.Request) {
	httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid query parameter", nil)
}
