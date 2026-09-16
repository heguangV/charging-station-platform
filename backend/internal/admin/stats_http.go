package admin

import (
	"errors"
	"net/http"
	"strconv"

	"github.com/heguangV/charging-station-platform/backend/internal/auth"
	"github.com/heguangV/charging-station-platform/backend/internal/httpapi"
)

// The statistics endpoints the console's operations pages read.
//
// They are reads with an admin session, like every other read on this boundary:
// an auditor may look at the numbers, and nothing here changes a row. That is
// also why they carry no idempotency key - a retried read is the same read.

// Stats paths, published in api/openapi.yaml.
const (
	RevenueStatsPath  = "/api/v1/admin/stats/revenue"
	ChargerStatsPath  = "/api/v1/admin/stats/chargers"
	OverviewStatsPath = "/api/v1/admin/stats/overview"
)

// registerStats attaches the statistics routes.
func (h *Handlers) registerStats(server interface {
	Register(pattern string, handler http.HandlerFunc)
}) {
	server.Register(RevenueStatsPath, h.auth.RequireRole(auth.RoleAdmin, h.revenueStats))
	server.Register(ChargerStatsPath, h.auth.RequireRole(auth.RoleAdmin, h.chargerStats))
	server.Register(OverviewStatsPath, h.auth.RequireRole(auth.RoleAdmin, h.overviewStats))
}

// revenueStats handles GET /api/v1/admin/stats/revenue.
func (h *Handlers) revenueStats(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodGet) {
		return
	}
	query, ok := h.parseRevenueQuery(w, r)
	if !ok {
		return
	}
	result, err := h.service.RevenueStats(r.Context(), query)
	if err != nil {
		writeStatsError(w, r, err)
		return
	}
	httpapi.WriteJSON(w, http.StatusOK, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    newRevenueStatsBody(result),
	})
}

// chargerStats handles GET /api/v1/admin/stats/chargers.
func (h *Handlers) chargerStats(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodGet) {
		return
	}
	stationID, ok := parseStationFilter(w, r)
	if !ok {
		return
	}
	result, err := h.service.ChargerStatus(r.Context(), stationID)
	if err != nil {
		writeStatsError(w, r, err)
		return
	}
	httpapi.WriteJSON(w, http.StatusOK, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    newChargerStatusBody(result),
	})
}

// overviewStats handles GET /api/v1/admin/stats/overview.
func (h *Handlers) overviewStats(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodGet) {
		return
	}
	result, err := h.service.Overview(r.Context())
	if err != nil {
		writeStatsError(w, r, err)
		return
	}
	httpapi.WriteJSON(w, http.StatusOK, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data: overviewBody{
			GeneratedAt:         result.GeneratedAt,
			TotalRevenueCent:    result.TotalRevenueCent,
			TotalChargeCount:    result.TotalChargeCount,
			FastChargeCount:     result.FastChargeCount,
			SlowChargeCount:     result.SlowChargeCount,
			RegisteredUserCount: result.RegisteredUserCount,
			StationCount:        result.StationCount,
			ActiveOrderCount:    result.ActiveOrderCount,
			Chargers:            newChargerStatusBody(result.Chargers),
		},
	})
}

// parseRevenueQuery reads and checks the revenue parameters.
//
// The range is required rather than defaulted: the platform cannot know which
// timezone the caller's "last seven days" is in, so guessing one would answer a
// question nobody asked. Both bounds are UTC Unix seconds.
func (h *Handlers) parseRevenueQuery(w http.ResponseWriter, r *http.Request) (RevenueQuery, bool) {
	query := r.URL.Query()

	fromAt, ok := parseRequiredInt64(w, r, query.Get("fromAt"), "fromAt")
	if !ok {
		return RevenueQuery{}, false
	}
	toAt, ok := parseRequiredInt64(w, r, query.Get("toAt"), "toAt")
	if !ok {
		return RevenueQuery{}, false
	}
	stationID, ok := parseStationFilter(w, r)
	if !ok {
		return RevenueQuery{}, false
	}
	bucket := query.Get("bucket")
	if bucket == "" {
		bucket = StatsBucketDay
	}

	result := RevenueQuery{FromAt: fromAt, ToAt: toAt, StationID: stationID, Bucket: bucket}
	if err := result.Validate(); err != nil {
		writeStatsError(w, r, err)
		return RevenueQuery{}, false
	}
	return result, true
}

// parseStationFilter reads the optional station filter shared by the two
// station-scoped statistics endpoints.
func parseStationFilter(w http.ResponseWriter, r *http.Request) (int64, bool) {
	raw := r.URL.Query().Get("stationId")
	if raw == "" {
		return 0, true
	}
	value, err := strconv.ParseInt(raw, 10, 64)
	if err != nil || value < 1 {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid station id", nil)
		return 0, false
	}
	return value, true
}

// parseRequiredInt64 reads one integer query parameter.
func parseRequiredInt64(w http.ResponseWriter, r *http.Request, raw, name string) (int64, bool) {
	if raw == "" {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, name+" is required", nil)
		return 0, false
	}
	value, err := strconv.ParseInt(raw, 10, 64)
	if err != nil {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, name+" must be an integer", nil)
		return 0, false
	}
	return value, true
}

// writeStatsError maps a statistics error to the shared envelope.
//
// Every statistics error describes the caller's query, so all of them are 400:
// none of them can be produced by a correct request, and answering 503 would
// tell the console to retry a query that will never succeed.
func writeStatsError(w http.ResponseWriter, r *http.Request, err error) {
	switch {
	case errors.Is(err, ErrInvalidStatsRange), errors.Is(err, ErrInvalidStatsBucket),
		errors.Is(err, ErrInvalidStatsStation):
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, err.Error(), nil)
	default:
		writeAdminError(w, r, err)
	}
}

// —— response bodies ——
//
// Separate from the service's own types so a change in how the platform stores
// an order cannot silently change what the console receives.

type revenueBucketBody struct {
	BucketStart int64 `json:"bucketStart"`
	AmountCent  int64 `json:"amountCent"`
	EnergyMwh   int64 `json:"energyMwh"`
	OrderCount  int64 `json:"orderCount"`
}

type revenueStatsBody struct {
	Items           []revenueBucketBody `json:"items"`
	TotalAmountCent int64               `json:"totalAmountCent"`
	TotalEnergyMwh  int64               `json:"totalEnergyMwh"`
	TotalOrderCount int64               `json:"totalOrderCount"`
}

type chargerStatusBody struct {
	IdleCount        int64   `json:"idleCount"`
	OccupiedCount    int64   `json:"occupiedCount"`
	FaultyCount      int64   `json:"faultyCount"`
	RestartingCount  int64   `json:"restartingCount"`
	DisabledCount    int64   `json:"disabledCount"`
	OperationalCount int64   `json:"operationalCount"`
	TotalCount       int64   `json:"totalCount"`
	HealthPercent    float64 `json:"healthPercent"`
}

type overviewBody struct {
	GeneratedAt         int64             `json:"generatedAt"`
	TotalRevenueCent    int64             `json:"totalRevenueCent"`
	TotalChargeCount    int64             `json:"totalChargeCount"`
	FastChargeCount     int64             `json:"fastChargeCount"`
	SlowChargeCount     int64             `json:"slowChargeCount"`
	RegisteredUserCount int64             `json:"registeredUserCount"`
	StationCount        int64             `json:"stationCount"`
	ActiveOrderCount    int64             `json:"activeOrderCount"`
	Chargers            chargerStatusBody `json:"chargers"`
}

// newRevenueStatsBody renders a revenue series.
//
// The item list is never nil: the console iterates it directly, and "the range
// had no settled orders" is a normal answer that must render as an empty table
// rather than as a client-side special case.
func newRevenueStatsBody(stats RevenueStats) revenueStatsBody {
	body := revenueStatsBody{
		Items:           make([]revenueBucketBody, 0, len(stats.Items)),
		TotalAmountCent: stats.TotalAmountCent,
		TotalEnergyMwh:  stats.TotalEnergyMwh,
		TotalOrderCount: stats.TotalOrderCount,
	}
	for _, item := range stats.Items {
		body.Items = append(body.Items, revenueBucketBody{
			BucketStart: item.BucketStart,
			AmountCent:  item.AmountCent,
			EnergyMwh:   item.EnergyMwh,
			OrderCount:  item.OrderCount,
		})
	}
	return body
}

func newChargerStatusBody(stats ChargerStatusStats) chargerStatusBody {
	return chargerStatusBody{
		IdleCount:        stats.IdleCount,
		OccupiedCount:    stats.OccupiedCount,
		FaultyCount:      stats.FaultyCount,
		RestartingCount:  stats.RestartingCount,
		DisabledCount:    stats.DisabledCount,
		OperationalCount: stats.OperationalCount,
		TotalCount:       stats.TotalCount,
		HealthPercent:    stats.HealthPercent,
	}
}
