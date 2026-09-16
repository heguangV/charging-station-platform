package postgres

import (
	"context"
	"database/sql"
	"math"
	"strings"

	"github.com/heguangV/charging-station-platform/backend/internal/station"
)

// The recommendation reads behind the assistant.
//
// They are a separate file rather than extra columns on the list query because
// they answer a different question: a recommendation has to state the price
// split and the charger mix, and the contract list deliberately carries only
// the cheapest total. The filter and distance expressions are shared with the
// list - not copied - so the two can never disagree about which stations match
// or how far away they are.

// summaryColumns is the projection both summary queries return, in scan order.
//
// Every column is aliased because the search query wraps this projection in a
// CTE and re-selects from it: an unaliased COALESCE would come out of the CTE
// named "coalesce", and the outer query would fail on a column that looks
// present.
//
// The price split comes from the same cheapest charger that decides the total,
// ordered by the same expression the list filter uses, so the parts always add
// up to the whole.
const summaryColumns = `s.id AS id, s.code AS code, s.name AS name, s.address AS address,
       s.status AS status,
       COALESCE(s.latitude::float8, 0) AS latitude,
       COALESCE(s.longitude::float8, 0) AS longitude,
       (SELECT count(*) FROM chargers c WHERE c.station_id = s.id) AS charger_count,
       (SELECT count(*) FROM chargers c WHERE c.station_id = s.id AND c.status = 'IDLE') AS idle_count,
       (SELECT count(*) FROM chargers c WHERE c.station_id = s.id AND c.status <> 'DISABLED') AS operational_count,
       (SELECT count(*) FROM chargers c WHERE c.station_id = s.id AND c.connector_type = 'DC') AS fast_count,
       (SELECT count(*) FROM chargers c WHERE c.station_id = s.id AND c.connector_type = 'AC') AS slow_count,
       COALESCE((SELECT string_agg(DISTINCT c.connector_type, ',' ORDER BY c.connector_type)
                 FROM chargers c WHERE c.station_id = s.id), '') AS charger_types,
       COALESCE(cheapest.price_per_kwh_cents, 0) AS electricity_price,
       COALESCE(cheapest.service_price_per_kwh_cents, 0) AS service_price`

// summaryCheapest joins the cheapest charger of a station.
const summaryCheapest = `
    LEFT JOIN LATERAL (
        SELECT c.price_per_kwh_cents, c.service_price_per_kwh_cents
        FROM chargers c
        WHERE c.station_id = s.id
        ORDER BY c.price_per_kwh_cents + c.service_price_per_kwh_cents, c.id
        LIMIT 1
    ) cheapest ON TRUE`

// summaryListParams expands the shared filter arguments for a summary search.
// The last element is include-disabled: the assistant answers a C-end user, so
// a disabled station is never a recommendation.
func summaryListParams(filter station.SummaryFilter) []any {
	var latitude, longitude any
	if filter.HasLocation {
		latitude = filter.Latitude
		longitude = filter.Longitude
	}
	return []any{
		latitude, longitude,
		likePattern(filter.Keyword),
		filter.ConnectorType,
		filter.MaxPriceCentPerKwh,
		filter.MinIdleChargers,
		filter.RadiusMeters,
		false,
	}
}

// scanSummary reads one row of summaryColumns.
//
// Charger types arrive as a comma-separated string rather than a driver array:
// string_agg keeps the query portable across the drivers this store may be
// pointed at, and the split is trivial.
func scanSummary(rows interface {
	Scan(dest ...any) error
}) (station.Summary, error) {
	var (
		summary              station.Summary
		latitude, longitude  float64
		chargerTypes         string
		electricity, service int64
		distance             sql.NullFloat64
	)
	if err := rows.Scan(&summary.ID, &summary.Code, &summary.Name, &summary.Address,
		&summary.Status, &latitude, &longitude,
		&summary.ChargerCount, &summary.IdleChargerCount, &summary.OperationalChargerCount,
		&summary.FastChargerCount, &summary.SlowChargerCount, &chargerTypes,
		&electricity, &service, &distance); err != nil {
		return station.Summary{}, err
	}
	summary.LatitudeE6 = toE6(latitude)
	summary.LongitudeE6 = toE6(longitude)
	summary.ElectricityPriceCentPerKwh = electricity
	summary.ServicePriceCentPerKwh = service
	summary.TotalPriceCentPerKwh = electricity + service
	if chargerTypes != "" {
		summary.ChargerTypes = strings.Split(chargerTypes, ",")
	}
	if distance.Valid {
		summary.DistanceMeter = int64(math.Round(distance.Float64))
		summary.HasDistance = true
	}
	return summary, nil
}

// SearchStationSummaries returns the recommendation view of a station search.
func (s *StationStore) SearchStationSummaries(ctx context.Context, filter station.SummaryFilter) ([]station.Summary, error) {
	args := append(summaryListParams(filter), filter.Limit)
	rows, err := s.db.QueryContext(ctx, `WITH base AS (
    SELECT `+summaryColumns+`,
           `+stationDistance+` AS distance
    FROM stations s`+summaryCheapest+`
    WHERE `+stationFilterWhere+`
)
SELECT b.id, b.code, b.name, b.address, b.status,
       COALESCE(b.latitude::float8, 0), COALESCE(b.longitude::float8, 0),
       b.charger_count, b.idle_count, b.operational_count, b.fast_count, b.slow_count,
       b.charger_types, b.electricity_price, b.service_price, b.distance
FROM base b
WHERE ($7 <= 0 OR b.distance <= $7::float8)
ORDER BY b.distance ASC NULLS LAST, b.id ASC
LIMIT $9`, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	summaries := make([]station.Summary, 0, filter.Limit)
	for rows.Next() {
		summary, err := scanSummary(rows)
		if err != nil {
			return nil, err
		}
		summaries = append(summaries, summary)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	return summaries, nil
}

// GetStationSummary returns one station with a page of its chargers.
//
// A disabled station is not found rather than returned: the C-end must not be
// offered one, and a "not found" is the honest answer to a request for
// something the user cannot use.
func (s *StationStore) GetStationSummary(ctx context.Context, stationID int64, chargerType string, chargerLimit int) (station.SummaryDetail, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT `+summaryColumns+`
FROM stations s`+summaryCheapest+`
WHERE s.id = $1 AND s.status <> 'DISABLED'`, stationID)
	if err != nil {
		return station.SummaryDetail{}, err
	}
	defer rows.Close()

	if !rows.Next() {
		if err := rows.Err(); err != nil {
			return station.SummaryDetail{}, err
		}
		return station.SummaryDetail{}, station.ErrStationNotFound
	}
	// The detail query has no distance: it is a lookup by id, not a search from
	// a position.
	summary, err := scanSummary(rows)
	if err != nil {
		return station.SummaryDetail{}, err
	}
	if err := rows.Err(); err != nil {
		return station.SummaryDetail{}, err
	}

	detail := station.SummaryDetail{Summary: summary}
	if err := s.db.QueryRowContext(ctx, `SELECT count(*) FROM chargers
WHERE station_id = $1 AND ($2 = '' OR connector_type = $2)`, stationID, chargerType).Scan(&detail.ChargerTotal); err != nil {
		return station.SummaryDetail{}, err
	}

	chargerRows, err := s.db.QueryContext(ctx, `SELECT code, connector_type, power_watt, status
FROM chargers
WHERE station_id = $1 AND ($2 = '' OR connector_type = $2)
ORDER BY id
LIMIT $3`, stationID, chargerType, chargerLimit)
	if err != nil {
		return station.SummaryDetail{}, err
	}
	defer chargerRows.Close()

	detail.Chargers = make([]station.ChargerInfo, 0, chargerLimit)
	for chargerRows.Next() {
		var charger station.ChargerInfo
		if err := chargerRows.Scan(&charger.Code, &charger.Type, &charger.PowerWatt, &charger.Status); err != nil {
			return station.SummaryDetail{}, err
		}
		detail.Chargers = append(detail.Chargers, charger)
	}
	if err := chargerRows.Err(); err != nil {
		return station.SummaryDetail{}, err
	}
	return detail, nil
}

// compile-time proof that the store satisfies the recommendation boundary.
var _ station.SummaryReader = (*StationStore)(nil)
