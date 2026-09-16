package admin

import (
	"encoding/json"
	"net/http"

	"github.com/heguangV/charging-station-platform/backend/internal/auth"
	"github.com/heguangV/charging-station-platform/backend/internal/httpapi"
)

// The endpoints of station editing and the fleet-wide tariff.
//
// Both follow the same policy as the operations beside them: reading is open to
// every administrator role, because an auditor has to be able to see what the
// platform charges, and writing is restricted to the write-capable roles,
// because both of these change something a customer sees.

// AdminStationsProfilePattern is the station edit route.
const AdminStationsProfilePattern = "/api/v1/admin/stations/{stationId}"

// AdminTariffsPattern is the fleet-wide tariff route.
const AdminTariffsPattern = "/api/v1/admin/tariffs"

// registerProfile attaches the station edit and fleet tariff routes.
func (h *Handlers) registerProfile(server interface {
	Register(pattern string, handler http.HandlerFunc)
}) {
	server.Register(AdminStationsProfilePattern, h.auth.RequireAdminWrite(h.updateStation))
	server.Register(AdminTariffsPattern, h.tariffProfileRoutes)
}

// tariffProfileRoutes dispatches GET and PUT on /admin/tariffs.
//
// One route pattern serves both methods, so the two policies are applied per
// branch rather than at registration.
func (h *Handlers) tariffProfileRoutes(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		h.auth.RequireRole(auth.RoleAdmin, h.getGlobalTariff)(w, r)
	case http.MethodPut:
		h.auth.RequireAdminWrite(h.updateGlobalTariff)(w, r)
	default:
		w.Header().Set("Allow", "GET, PUT")
		httpapi.WriteError(w, r, http.StatusMethodNotAllowed, httpapi.CodeMethodNotAllowed, "method not allowed", nil)
	}
}

// updateStationRequest is the editable profile of a station.
//
// The code and the status are deliberately absent: this endpoint does not
// accept them, and a client that sends them is not silently obeyed.
type updateStationRequest struct {
	Name        string `json:"name"`
	Address     string `json:"address"`
	LatitudeE6  int64  `json:"latitudeE6"`
	LongitudeE6 int64  `json:"longitudeE6"`
}

// updateStation handles PUT /api/v1/admin/stations/{stationId}.
func (h *Handlers) updateStation(w http.ResponseWriter, r *http.Request) {
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
	body, ok := readBody(w, r)
	if !ok {
		return
	}
	// The coordinates are integers in the contract. Decoding with UseNumber is
	// not needed here because they are typed fields, but a body that carries a
	// fractional coordinate must be refused rather than truncated: 39.977680 is
	// a place, 39 is a different one.
	var request updateStationRequest
	if err := json.Unmarshal(body, &request); err != nil {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid request body", nil)
		return
	}

	result, err := h.service.UpdateStation(r.Context(), UpdateStationCommand{
		AdminID:     identity.ID,
		StationID:   stationID,
		Name:        request.Name,
		Address:     request.Address,
		LatitudeE6:  request.LatitudeE6,
		LongitudeE6: request.LongitudeE6,
		TraceID:     httpapi.RequestID(r.Context()),
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

// getGlobalTariff handles GET /api/v1/admin/tariffs.
func (h *Handlers) getGlobalTariff(w http.ResponseWriter, r *http.Request) {
	result, err := h.service.GlobalTariff(r.Context())
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

// updateGlobalTariffRequest is the complete tariff the fleet should hold.
type updateGlobalTariffRequest struct {
	ElectricityPriceCent int64  `json:"electricityPriceCentPerKwh"`
	ServicePriceCent     int64  `json:"servicePriceCentPerKwh"`
	OffPeakPriceCent     *int64 `json:"offPeakElectricityPriceCentPerKwh"`
	OffPeakStartHour     *int16 `json:"offPeakStartHour"`
	OffPeakEndHour       *int16 `json:"offPeakEndHour"`
	Reason               string `json:"reason"`
}

// updateGlobalTariff handles PUT /api/v1/admin/tariffs.
func (h *Handlers) updateGlobalTariff(w http.ResponseWriter, r *http.Request) {
	identity, ok := identityFrom(w, r)
	if !ok {
		return
	}
	body, ok := readBody(w, r)
	if !ok {
		return
	}
	var request updateGlobalTariffRequest
	if err := json.Unmarshal(body, &request); err != nil {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid request body", nil)
		return
	}

	result, err := h.service.UpdateGlobalTariff(r.Context(), GlobalTariffUpdate{
		AdminID:              identity.ID,
		ElectricityPriceCent: request.ElectricityPriceCent,
		ServicePriceCent:     request.ServicePriceCent,
		OffPeakPriceCent:     request.OffPeakPriceCent,
		OffPeakStartHour:     request.OffPeakStartHour,
		OffPeakEndHour:       request.OffPeakEndHour,
		Reason:               request.Reason,
		TraceID:              httpapi.RequestID(r.Context()),
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
