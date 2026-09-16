package review

import (
	"encoding/json"
	"errors"
	"net/http"
	"strconv"

	"github.com/heguangV/charging-station-platform/backend/internal/auth"
	"github.com/heguangV/charging-station-platform/backend/internal/httpapi"
)

// identityProvider supplies RequireIdentity and RequireRole; auth.Handlers
// implements it.
type identityProvider interface {
	RequireIdentity(next http.HandlerFunc) http.HandlerFunc
	RequireRole(role string, next http.HandlerFunc) http.HandlerFunc
}

// adminGuard marks the admin-approval operations (SUPER_ADMIN/OPERATOR).
type adminGuard interface {
	RequireAdminWrite(next http.HandlerFunc) http.HandlerFunc
}

// Handlers expose the review and appeal endpoints.
type Handlers struct {
	service *Service
	auth    identityProvider
	admin   adminGuard
}

// NewHandlers binds the handlers to the service and middleware.
func NewHandlers(service *Service, auth identityProvider, admin adminGuard) (*Handlers, error) {
	if service == nil {
		return nil, errors.New("review: service is required")
	}
	if auth == nil {
		return nil, errors.New("review: auth middleware is required")
	}
	if admin == nil {
		return nil, errors.New("review: admin middleware is required")
	}
	return &Handlers{service: service, auth: auth, admin: admin}, nil
}

// Register attaches every review and appeal route to the server.
func (h *Handlers) Register(server interface {
	Register(pattern string, handler http.HandlerFunc)
}) {
	server.Register("/api/v1/orders/{orderNo}/review", h.auth.RequireIdentity(h.reviewRoutes))
	server.Register("/api/v1/stations/{stationId}/reviews", h.auth.RequireIdentity(h.wall))
	server.Register("/api/v1/orders/{orderNo}/appeal", h.auth.RequireIdentity(h.appealRoutes))
	server.Register("/api/v1/admin/appeals", h.auth.RequireRole(auth.RoleAdmin, h.adminListAppeals))
	server.Register("/api/v1/admin/appeals/{appealId}/approve", h.admin.RequireAdminWrite(h.adminApprove))
	server.Register("/api/v1/admin/appeals/{appealId}/reject", h.admin.RequireAdminWrite(h.adminReject))
}

type reviewRequest struct {
	Stars   int    `json:"stars"`
	Comment string `json:"comment"`
}

type appealRequest struct {
	Reason string `json:"reason"`
}

// reviewRoutes dispatches GET (own review) and POST (create) on the order
// review resource.
func (h *Handlers) reviewRoutes(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		h.getReview(w, r)
	case http.MethodPost:
		h.createReview(w, r)
	default:
		w.Header().Set("Allow", "GET, POST")
		httpapi.WriteError(w, r, http.StatusMethodNotAllowed, httpapi.CodeMethodNotAllowed, "method not allowed", nil)
	}
}

func (h *Handlers) createReview(w http.ResponseWriter, r *http.Request) {
	identity, ok := identityFromReview(w, r)
	if !ok {
		return
	}
	var request reviewRequest
	if err := decodeJSONReview(w, r, &request); err != nil {
		return
	}

	view, err := h.service.Create(r.Context(), identity.ID, r.PathValue("orderNo"), request.Stars, request.Comment)
	if err != nil {
		writeReviewError(w, r, err)
		return
	}
	httpapi.WriteJSON(w, http.StatusCreated, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    view,
	})
}

func (h *Handlers) getReview(w http.ResponseWriter, r *http.Request) {
	identity, ok := identityFromReview(w, r)
	if !ok {
		return
	}
	view, err := h.service.Get(r.Context(), identity.ID, r.PathValue("orderNo"))
	if err != nil {
		writeReviewError(w, r, err)
		return
	}
	httpapi.WriteJSON(w, http.StatusOK, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    view,
	})
}

// wall handles GET /api/v1/stations/{stationId}/reviews (UC-U-12 评论墙:
// login required, newest first, authors masked).
func (h *Handlers) wall(w http.ResponseWriter, r *http.Request) {
	if !requireMethodReview(w, r, http.MethodGet) {
		return
	}
	stationID, err := strconv.ParseInt(r.PathValue("stationId"), 10, 64)
	if err != nil || stationID < 1 {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid station id", nil)
		return
	}
	page, pageSize, perr := httpapi.ParsePagination(r)
	if perr != nil {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid query parameter", nil)
		return
	}

	result, err := h.service.Wall(r.Context(), WallFilter{StationID: stationID, Page: page, PageSize: pageSize})
	if err != nil {
		writeReviewError(w, r, err)
		return
	}
	httpapi.WriteJSON(w, http.StatusOK, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    result,
	})
}

// appealRoutes dispatches GET (the caller's own appeal) and POST (create) on
// the order appeal resource, mirroring the review resource.
func (h *Handlers) appealRoutes(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		h.getAppeal(w, r)
	case http.MethodPost:
		h.createAppeal(w, r)
	default:
		w.Header().Set("Allow", "GET, POST")
		httpapi.WriteError(w, r, http.StatusMethodNotAllowed, httpapi.CodeMethodNotAllowed, "method not allowed", nil)
	}
}

// getAppeal handles GET /api/v1/orders/{orderNo}/appeal: the caller's own
// appeal, so the app can show 审核中/已通过/已驳回 instead of offering the form
// again. No appeal yet is a 404, the same shape the review resource uses.
func (h *Handlers) getAppeal(w http.ResponseWriter, r *http.Request) {
	identity, ok := identityFromReview(w, r)
	if !ok {
		return
	}
	view, err := h.service.GetAppeal(r.Context(), identity.ID, r.PathValue("orderNo"))
	if err != nil {
		writeReviewError(w, r, err)
		return
	}
	httpapi.WriteJSON(w, http.StatusOK, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    view,
	})
}

// createAppeal handles POST /api/v1/orders/{orderNo}/appeal.
func (h *Handlers) createAppeal(w http.ResponseWriter, r *http.Request) {
	identity, ok := identityFromReview(w, r)
	if !ok {
		return
	}
	var request appealRequest
	if err := decodeJSONReview(w, r, &request); err != nil {
		return
	}

	view, err := h.service.CreateAppeal(r.Context(), identity.ID, r.PathValue("orderNo"), request.Reason)
	if err != nil {
		writeReviewError(w, r, err)
		return
	}
	httpapi.WriteJSON(w, http.StatusCreated, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    view,
	})
}

// adminListAppeals handles GET /api/v1/admin/appeals.
func (h *Handlers) adminListAppeals(w http.ResponseWriter, r *http.Request) {
	if !requireMethodReview(w, r, http.MethodGet) {
		return
	}
	page, pageSize, perr := httpapi.ParsePagination(r)
	if perr != nil {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid query parameter", nil)
		return
	}

	result, err := h.service.Appeals(r.Context(), AppealFilter{
		Status: r.URL.Query().Get("status"), Page: page, PageSize: pageSize,
	})
	if err != nil {
		writeReviewError(w, r, err)
		return
	}
	httpapi.WriteJSON(w, http.StatusOK, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    result,
	})
}

// adminApprove handles POST /api/v1/admin/appeals/{appealId}/approve.
func (h *Handlers) adminApprove(w http.ResponseWriter, r *http.Request) {
	if !requireMethodReview(w, r, http.MethodPost) {
		return
	}
	identity, ok := identityFromReview(w, r)
	if !ok {
		return
	}
	appealID, err := strconv.ParseInt(r.PathValue("appealId"), 10, 64)
	if err != nil || appealID < 1 {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid appeal id", nil)
		return
	}

	if err := h.service.Approve(r.Context(), appealID, identity.ID); err != nil {
		writeReviewError(w, r, err)
		return
	}
	view, err := h.service.store.GetAppeal(r.Context(), appealID)
	if err != nil {
		writeReviewError(w, r, err)
		return
	}
	httpapi.WriteJSON(w, http.StatusOK, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    view,
	})
}

// adminReject handles POST /api/v1/admin/appeals/{appealId}/reject: the appeal is dismissed with the
// operator's reason, and the order and the wallet stay untouched (unlike approval, which cancels
// the order and refunds). Repeating the decision is a no-op that returns the current state.
func (h *Handlers) adminReject(w http.ResponseWriter, r *http.Request) {
	if !requireMethodReview(w, r, http.MethodPost) {
		return
	}
	identity, ok := identityFromReview(w, r)
	if !ok {
		return
	}
	appealID, err := strconv.ParseInt(r.PathValue("appealId"), 10, 64)
	if err != nil || appealID < 1 {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid appeal id", nil)
		return
	}
	var request appealRequest
	if err := decodeJSONReview(w, r, &request); err != nil {
		return
	}

	if err := h.service.Reject(r.Context(), appealID, identity.ID, request.Reason); err != nil {
		writeReviewError(w, r, err)
		return
	}
	view, err := h.service.store.GetAppeal(r.Context(), appealID)
	if err != nil {
		writeReviewError(w, r, err)
		return
	}
	httpapi.WriteJSON(w, http.StatusOK, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    view,
	})
}

func identityFromReview(w http.ResponseWriter, r *http.Request) (auth.Identity, bool) {
	identity, ok := auth.IdentityFromContext(r.Context())
	if !ok {
		httpapi.WriteError(w, r, http.StatusUnauthorized, httpapi.CodeUnauthorized, "session is missing or expired", nil)
		return auth.Identity{}, false
	}
	return identity, true
}

func requireMethodReview(w http.ResponseWriter, r *http.Request, method string) bool {
	if r.Method == method {
		return true
	}
	w.Header().Set("Allow", method)
	httpapi.WriteError(w, r, http.StatusMethodNotAllowed, httpapi.CodeMethodNotAllowed, "method not allowed", nil)
	return false
}

func decodeJSONReview(w http.ResponseWriter, r *http.Request, target any) error {
	decoder := json.NewDecoder(http.MaxBytesReader(w, r.Body, 16<<10))
	if err := decoder.Decode(target); err != nil {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid request body", nil)
		return err
	}
	return nil
}

func writeReviewError(w http.ResponseWriter, r *http.Request, err error) {
	switch {
	case errors.Is(err, ErrInvalidStars), errors.Is(err, ErrInvalidComment),
		errors.Is(err, ErrInvalidReason):
		httpapi.WriteError(w, r, http.StatusBadRequest, 2, err.Error(), nil)
	case errors.Is(err, ErrOrderNotReviewable), errors.Is(err, ErrOrderNotAppealable):
		httpapi.WriteError(w, r, http.StatusConflict, 15, "order is not in a reviewable or appealable state", nil)
	case errors.Is(err, ErrReviewConflict), errors.Is(err, ErrAppealConflict), errors.Is(err, ErrAppealAlreadyApproved):
		httpapi.WriteError(w, r, http.StatusConflict, 5, "already exists with different content or state", nil)
	case errors.Is(err, ErrNotFound), errors.Is(err, ErrNotOrderOwner):
		httpapi.WriteError(w, r, http.StatusNotFound, 4, "not found", nil)
	default:
		httpapi.WriteError(w, r, http.StatusServiceUnavailable, 3, "review service is temporarily unavailable", nil)
	}
}
