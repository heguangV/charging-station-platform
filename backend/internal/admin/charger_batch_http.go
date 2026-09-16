package admin

import (
	"encoding/json"
	"net/http"

	"github.com/heguangV/charging-station-platform/backend/internal/httpapi"
)

// The batch device endpoint.
//
// It is a write on the same policy as the operations beside it, and it needs an
// idempotency key like every other write on this boundary: a batch takes long
// enough to submit that a retry is a real possibility, and a retried batch that
// created a second set of devices would be discovered in the cabinet.

// ChargerBatchPath is the batch creation route.
const ChargerBatchPath = "/api/v1/admin/chargers/batch"

// registerChargerBatch attaches the batch route.
func (h *Handlers) registerChargerBatch(server interface {
	Register(pattern string, handler http.HandlerFunc)
}) {
	server.Register(ChargerBatchPath, h.auth.RequireAdminWrite(h.createChargers))
}

// createChargersRequest is the batch payload.
//
// It carries only what an operator types in. Status, price and version are the
// platform's to decide, and the console's 0/1 connector shorthand is translated
// at the client: the contract speaks AC and DC, which is what the column holds
// and what every other charger payload says.
type createChargersRequest struct {
	StationID int64 `json:"stationId"`
	Chargers  []struct {
		Code          string `json:"code"`
		ConnectorType string `json:"connectorType"`
		PowerWatt     int64  `json:"powerWatt"`
	} `json:"chargers"`
}

// createChargers handles POST /api/v1/admin/chargers/batch.
func (h *Handlers) createChargers(w http.ResponseWriter, r *http.Request) {
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
	// The key is checked before the body is decoded: a request that carries no
	// key cannot be replayed, and decoding a batch that will be refused anyway
	// wastes the one thing a batch is expensive in.
	key, ok := requireIdempotencyKey(w, r)
	if !ok {
		return
	}

	var request createChargersRequest
	if err := json.Unmarshal(body, &request); err != nil {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid request body", nil)
		return
	}
	drafts := make([]ChargerDraft, 0, len(request.Chargers))
	for _, charger := range request.Chargers {
		drafts = append(drafts, ChargerDraft{
			Code:          charger.Code,
			ConnectorType: charger.ConnectorType,
			PowerWatt:     charger.PowerWatt,
		})
	}

	result, err := h.service.CreateChargers(r.Context(), CreateChargersCommand{
		AdminID:        identity.ID,
		StationID:      request.StationID,
		Chargers:       drafts,
		IdempotencyKey: key,
		RequestHash:    requestHash(r, body),
		TraceID:        httpapi.RequestID(r.Context()),
	})
	if err != nil {
		writeAdminError(w, r, err)
		return
	}
	// 201 Created: the batch produced resources, and the response carries them.
	httpapi.WriteJSON(w, http.StatusCreated, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    result,
	})
}
