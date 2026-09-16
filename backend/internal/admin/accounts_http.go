package admin

import (
	"encoding/json"
	"net/http"
	"strconv"

	"github.com/heguangV/charging-station-platform/backend/internal/auth"
	"github.com/heguangV/charging-station-platform/backend/internal/httpapi"
)

type createAdminAccountRequest struct {
	Username string `json:"username"`
	Password string `json:"password"`
	Reason   string `json:"reason"`
}

type setAdminAccountStatusRequest struct {
	Status  int    `json:"status"`
	Version int64  `json:"version"`
	Reason  string `json:"reason"`
}

func (h *Handlers) adminAccounts(w http.ResponseWriter, r *http.Request) {
	identity, ok := requireSuperAdmin(w, r)
	if !ok {
		return
	}
	switch r.Method {
	case http.MethodGet:
		page, pageSize, ok := h.parsePagination(w, r)
		if !ok {
			return
		}
		result, err := h.service.Accounts(r.Context(), page, pageSize)
		if err != nil {
			writeAdminError(w, r, err)
			return
		}
		writePage(w, r, http.StatusOK, result)
	case http.MethodPost:
		body, ok := readBody(w, r)
		if !ok {
			return
		}
		var request createAdminAccountRequest
		if err := json.Unmarshal(body, &request); err != nil {
			httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid request body", nil)
			return
		}
		result, err := h.service.CreateAccount(r.Context(), identity.ID, request.Username, request.Password, request.Reason, httpapi.RequestID(r.Context()))
		if err != nil {
			writeAdminError(w, r, err)
			return
		}
		writePage(w, r, http.StatusCreated, result)
	default:
		w.Header().Set("Allow", "GET, POST")
		httpapi.WriteError(w, r, http.StatusMethodNotAllowed, httpapi.CodeMethodNotAllowed, "method not allowed", nil)
	}
}

func (h *Handlers) adminAccountStatus(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPut {
		w.Header().Set("Allow", http.MethodPut)
		httpapi.WriteError(w, r, http.StatusMethodNotAllowed, httpapi.CodeMethodNotAllowed, "method not allowed", nil)
		return
	}
	identity, ok := requireSuperAdmin(w, r)
	if !ok {
		return
	}
	accountID, err := strconv.ParseInt(r.PathValue("accountId"), 10, 64)
	if err != nil || accountID < 1 {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid administrator id", nil)
		return
	}
	body, ok := readBody(w, r)
	if !ok {
		return
	}
	var request setAdminAccountStatusRequest
	if err := json.Unmarshal(body, &request); err != nil {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid request body", nil)
		return
	}
	result, err := h.service.SetAccountStatus(r.Context(), SetAdminAccountStatusCommand{
		ActorID: identity.ID, AccountID: accountID, Status: request.Status, Version: request.Version,
		Reason: request.Reason, RequestID: httpapi.RequestID(r.Context()),
	})
	if err != nil {
		writeAdminError(w, r, err)
		return
	}
	writePage(w, r, http.StatusOK, result)
}

func requireSuperAdmin(w http.ResponseWriter, r *http.Request) (auth.Identity, bool) {
	identity, ok := identityFrom(w, r)
	if !ok {
		return auth.Identity{}, false
	}
	if identity.AdminRole != auth.AdminRoleSuper {
		httpapi.WriteError(w, r, http.StatusForbidden, httpapi.CodeForbidden, "super administrator role is required", nil)
		return auth.Identity{}, false
	}
	return identity, true
}
