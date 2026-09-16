package postgres

import (
	"context"

	"github.com/heguangV/charging-station-platform/backend/internal/admin"
)

// The statistics reads behind the console's operations pages.
//
// They aggregate what the platform already stores rather than maintaining a
// separate metrics table. The old service refreshed an hourly_metrics table on
// a timer and served a snapshot built from it; that bought fast reads at the
// cost of a second source of truth that could be stale, and it does not survive
// the move to PostgreSQL, where the aggregate over the orders table is a single
// indexed scan.
//
// Revenue is the bill of a COMPLETED order, attributed to stopped_at. That is
// the instant the stop receipt froze the amount, and unlike created_at or
// updated_at it cannot move afterwards, so a figure already reported never
// changes underneath a reader.

// statsRevenueWhere is the shared filter of both revenue queries.
//
// Placeholders: $1 from, $2 to, $3 station (0 = every station). The range is
// half-open so consecutive windows neither double-count nor drop the boundary
// second.
const statsRevenueWhere = `status = 'COMPLETED'
      AND stopped_at IS NOT NULL
      AND stopped_at >= to_timestamp($1::double precision)
      AND stopped_at < to_timestamp($2::double precision)
      AND ($3::bigint = 0 OR station_id = $3::bigint)`

// RevenueTotals sums the whole range.
func (s *AdminStore) RevenueTotals(ctx context.Context, query admin.RevenueQuery) (admin.RevenueTotals, error) {
	var totals admin.RevenueTotals
	// energy_wh is stored in watt-hours and reported in milliwatt-hours: the
	// contract's energy unit is the same one the orders endpoint uses, so a
	// client converts once and never has to know which figure came from where.
	if err := s.db.QueryRowContext(ctx, `SELECT
    COALESCE(SUM(amount_cents), 0),
    COALESCE(SUM(energy_wh), 0) * 1000,
    COUNT(*)
FROM charging_orders
WHERE `+statsRevenueWhere,
		query.FromAt, query.ToAt, query.StationID,
	).Scan(&totals.AmountCent, &totals.EnergyMwh, &totals.OrderCount); err != nil {
		return admin.RevenueTotals{}, err
	}
	return totals, nil
}

// RevenueBuckets groups the range into buckets aligned to the query's start.
//
// The alignment is relative to FromAt and not to the epoch because the client
// computes its own day boundaries: its "today" begins at its local midnight,
// which is rarely a UTC midnight. Aligning to the range makes each bucket
// exactly one of the client's days; epoch alignment would cut the first local
// day in half and show the remainder as a quiet day.
func (s *AdminStore) RevenueBuckets(ctx context.Context, query admin.RevenueQuery) ([]admin.RevenueBucket, error) {
	width := query.BucketSeconds()
	rows, err := s.db.QueryContext(ctx, `SELECT
    ($1::bigint + (floor(extract(epoch FROM (stopped_at - to_timestamp($1::double precision))) / $4::bigint) * $4::bigint))::bigint AS bucket_start,
    COALESCE(SUM(amount_cents), 0),
    COALESCE(SUM(energy_wh), 0) * 1000,
    COUNT(*)
FROM charging_orders
WHERE `+statsRevenueWhere+`
GROUP BY bucket_start
ORDER BY bucket_start`,
		query.FromAt, query.ToAt, query.StationID, width)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	buckets := make([]admin.RevenueBucket, 0, 32)
	for rows.Next() {
		var bucket admin.RevenueBucket
		if err := rows.Scan(&bucket.BucketStart, &bucket.AmountCent, &bucket.EnergyMwh, &bucket.OrderCount); err != nil {
			return nil, err
		}
		buckets = append(buckets, bucket)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	return buckets, nil
}

// ChargerCounts censuses the fleet in one pass.
//
// Every charger is counted, including a disabled one: an operator asking how
// many devices exist is not asking how many are usable, and the health figure
// is only meaningful against the whole fleet.
func (s *AdminStore) ChargerCounts(ctx context.Context, stationID int64) (admin.ChargerCounts, error) {
	var counts admin.ChargerCounts
	if err := s.db.QueryRowContext(ctx, `SELECT
    COUNT(*) FILTER (WHERE status = 'IDLE'),
    COUNT(*) FILTER (WHERE status = 'OCCUPIED'),
    COUNT(*) FILTER (WHERE status = 'FAULT'),
    COUNT(*) FILTER (WHERE status = 'RESTARTING'),
    COUNT(*) FILTER (WHERE status = 'DISABLED'),
    COUNT(*)
FROM chargers
WHERE ($1::bigint = 0 OR station_id = $1::bigint)`, stationID,
	).Scan(&counts.Idle, &counts.Occupied, &counts.Faulty,
		&counts.Restarting, &counts.Disabled, &counts.Total); err != nil {
		return admin.ChargerCounts{}, err
	}
	return counts, nil
}

// FleetCounts censuses accounts, stations, orders and lifetime revenue.
//
// The four scalars are separate subqueries rather than joins: joining an order
// aggregate to an account count would multiply one by the other, which is the
// classic way to report a number that is wrong by a factor nobody notices.
func (s *AdminStore) FleetCounts(ctx context.Context) (admin.FleetCounts, error) {
	var counts admin.FleetCounts
	err := s.db.QueryRowContext(ctx, `SELECT
    (SELECT COALESCE(SUM(amount_cents), 0) FROM charging_orders WHERE status = 'COMPLETED'),
    (SELECT COUNT(*) FROM charging_orders WHERE status = 'COMPLETED'),
    (SELECT COUNT(*) FROM charging_orders o
        JOIN chargers c ON c.id = o.charger_id
        WHERE o.status = 'COMPLETED' AND c.connector_type = 'DC'),
    (SELECT COUNT(*) FROM charging_orders o
        JOIN chargers c ON c.id = o.charger_id
        WHERE o.status = 'COMPLETED' AND c.connector_type = 'AC'),
    (SELECT COUNT(*) FROM user_accounts),
    (SELECT COUNT(*) FROM stations),
    (SELECT COUNT(*) FROM charging_orders
        WHERE status IN ('CREATED', 'STARTING', 'CHARGING', 'STOPPING'))`,
	).Scan(&counts.TotalRevenueCent, &counts.CompletedOrders, &counts.FastOrders,
		&counts.SlowOrders, &counts.RegisteredUsers, &counts.Stations, &counts.ActiveOrders)
	if err != nil {
		return admin.FleetCounts{}, err
	}
	return counts, nil
}

// compile-time proof that the store satisfies the admin boundary.
var _ admin.Store = (*AdminStore)(nil)
