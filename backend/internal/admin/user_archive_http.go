package admin

import (
	"encoding/json"
	"io"
	"net/http"

	"github.com/heguangV/charging-station-platform/backend/internal/auth"
	"github.com/heguangV/charging-station-platform/backend/internal/httpapi"
)

// The manual user archive endpoints.
//
// Creating an account is a write on the same policy as the operations beside
// it, and it needs an idempotency key like every other write on this
// boundary: an operator enrolling a roster works from a list, and a retried
// submission that created a second set of accounts would be discovered in the
// user list, not in an error.

// maxUserBatchBodyBytes bounds a roster page's body. The JSON limit beside it
// (16 KiB) fits one object comfortably, but a thousand drafts with a full
// display name each do not, and a limit that refuses a legal batch mid-way
// through is worse than a larger one.
const maxUserBatchBodyBytes = 512 << 10

// UserBatchPath is the roster-page route.
const UserBatchPath = "/api/v1/admin/users/batch"

// registerUserArchive attaches the roster-page route. The single-account POST
// rides the existing /admin/users pattern through the users dispatcher.
func (h *Handlers) registerUserArchive(server interface {
	Register(pattern string, handler http.HandlerFunc)
}) {
	server.Register(UserBatchPath, h.auth.RequireAdminWrite(h.createUsers))
}

// users routes GET /api/v1/admin/users (the page listing) and POST (archive
// one account, SUPER_ADMIN/OPERATOR only). GET and POST share one route
// pattern, so the write check runs in the handler: read-only auditors must
// not archive accounts.
func (h *Handlers) users(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		h.listUsers(w, r)
	case http.MethodPost:
		h.createUser(w, r)
	default:
		w.Header().Set("Allow", "GET, POST")
		httpapi.WriteError(w, r, http.StatusMethodNotAllowed, httpapi.CodeMethodNotAllowed, "method not allowed", nil)
	}
}

// createUserRequest is the single-account payload.
type createUserRequest struct {
	Phone       string `json:"phone"`
	DisplayName string `json:"displayName"`
}

// createUser handles POST /api/v1/admin/users.
func (h *Handlers) createUser(w http.ResponseWriter, r *http.Request) {
	identity, ok := identityFrom(w, r)
	if !ok {
		return
	}
	if !auth.AdminCanWrite(identity) {
		httpapi.WriteError(w, r, http.StatusForbidden, httpapi.CodeForbidden, "insufficient permission for administrative writes", nil)
		return
	}
	body, ok := readBody(w, r)
	if !ok {
		return
	}
	// The key is checked before the body is decoded, for the same reason the
	// batch endpoint does it: a request without a key cannot be replayed.
	key, ok := requireIdempotencyKey(w, r)
	if !ok {
		return
	}

	var request createUserRequest
	if err := json.Unmarshal(body, &request); err != nil {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid request body", nil)
		return
	}

	record, err := h.service.CreateUser(r.Context(), CreateUserCommand{
		AdminID:        identity.ID,
		User:           UserDraft{Phone: request.Phone, DisplayName: request.DisplayName},
		IdempotencyKey: key,
		RequestHash:    requestHash(r, body),
		TraceID:        httpapi.RequestID(r.Context()),
	})
	if err != nil {
		writeAdminError(w, r, err)
		return
	}
	// 201 Created: the request produced an account, and the response carries it.
	httpapi.WriteJSON(w, http.StatusCreated, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    record,
	})
}

// createUsersRequest is the roster-page payload.
type createUsersRequest struct {
	Users []createUserRequest `json:"users"`
}

// createUsers handles POST /api/v1/admin/users/batch.
func (h *Handlers) createUsers(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodPost) {
		return
	}
	identity, ok := identityFrom(w, r)
	if !ok {
		return
	}
	body, ok := readBodyLimit(w, r, maxUserBatchBodyBytes)
	if !ok {
		return
	}
	key, ok := requireIdempotencyKey(w, r)
	if !ok {
		return
	}

	var request createUsersRequest
	if err := json.Unmarshal(body, &request); err != nil {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid request body", nil)
		return
	}
	drafts := make([]UserDraft, 0, len(request.Users))
	for _, user := range request.Users {
		drafts = append(drafts, UserDraft{Phone: user.Phone, DisplayName: user.DisplayName})
	}

	result, err := h.service.CreateUsers(r.Context(), CreateUsersCommand{
		AdminID:        identity.ID,
		Users:          drafts,
		IdempotencyKey: key,
		RequestHash:    requestHash(r, body),
		TraceID:        httpapi.RequestID(r.Context()),
	})
	if err != nil {
		writeAdminError(w, r, err)
		return
	}
	httpapi.WriteJSON(w, http.StatusCreated, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    result,
	})
}

// readBodyLimit reads and bounds a JSON body with a caller-supplied limit.
func readBodyLimit(w http.ResponseWriter, r *http.Request, limit int64) ([]byte, bool) {
	body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, limit))
	if err != nil {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid request body", nil)
		return nil, false
	}
	return body, true
}
