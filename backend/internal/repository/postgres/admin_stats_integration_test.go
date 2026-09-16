package postgres

import (
	"context"
	"database/sql"
	"testing"

	"github.com/heguangV/charging-station-platform/backend/internal/admin"
)

// The statistics SQL, against a real PostgreSQL.
//
// These queries are the part of the feature a unit test cannot reach: the
// bucket alignment, the unit conversion and the FILTER counts are all decided
// by the database, and every one of them has a way of being subtly wrong that
// compiles and returns numbers. A wrong bucket boundary produces a chart that
// looks plausible and reports the wrong days.

// seedStatsFixture creates one station with three chargers and four orders.
//
// The orders are placed on exact hour boundaries so the expected buckets can be
// stated as integers: two completed orders an hour apart, one completed order
// far outside the window, and one cancelled order that no revenue query may
// count.
func seedStatsFixture(t *testing.T, db *sql.DB, ctx context.Context) (int64, int64, int64, func()) {
	t.Helper()
	suffix := uniqueSuffix(t)

	var stationID int64
	if err := db.QueryRowContext(ctx,
		`INSERT INTO stations (code, name, address, status) VALUES ($1, $2, $3, 'OPEN') RETURNING id`,
		"ST-STAT-"+suffix, "统计站"+suffix, "统计地址").Scan(&stationID); err != nil {
		t.Fatalf("insert station: %v", err)
	}

	// IDLE, FAULT and DISABLED: two operational, one not, which is the health
	// rule this feature has to keep.
	chargerStatuses := []struct {
		code      string
		connector string
		status    string
	}{
		{"S1-" + suffix, "DC", "IDLE"},
		{"S2-" + suffix, "AC", "OCCUPIED"},
		{"S3-" + suffix, "DC", "FAULT"},
		{"S4-" + suffix, "AC", "DISABLED"},
	}
	chargerIDs := make([]int64, 0, len(chargerStatuses))
	for _, charger := range chargerStatuses {
		var id int64
		if err := db.QueryRowContext(ctx,
			`INSERT INTO chargers (station_id, code, connector_type, power_watt, status)
			 VALUES ($1, $2, $3, 7000, $4) RETURNING id`,
			stationID, charger.code, charger.connector, charger.status).Scan(&id); err != nil {
			t.Fatalf("insert charger: %v", err)
		}
		chargerIDs = append(chargerIDs, id)
	}

	var userID int64
	if err := db.QueryRowContext(ctx,
		`INSERT INTO user_accounts (phone, display_name, password_hash) VALUES ($1, '', 'x') RETURNING id`,
		"139"+suffix).Scan(&userID); err != nil {
		t.Fatalf("insert user: %v", err)
	}

	// Orders. Bucket boundaries are stated in the same units the query uses so
	// the expected series is an integer arithmetic fact.
	orders := []struct {
		orderNo      string
		chargerIndex int
		status       string
		amountCent   int64
		energyWh     int64
		stoppedHours int64 // hours after the window start; -1 means outside
	}{
		{"ORD-STAT-A-" + suffix, 0, "COMPLETED", 1000, 2000, 0},
		{"ORD-STAT-B-" + suffix, 0, "COMPLETED", 2000, 3000, 1},
		{"ORD-STAT-C-" + suffix, 1, "COMPLETED", 500, 500, 3},
		{"ORD-STAT-D-" + suffix, 2, "CANCELLED", 9999, 9999, 1},
	}
	// The window the test queries over: 2026-01-01T00:00:00Z for four hours.
	const windowStart = int64(1767225600)
	for _, order := range orders {
		var stoppedAt any
		if order.stoppedHours >= 0 {
			stoppedAt = windowStart + order.stoppedHours*3600
		}
		if _, err := db.ExecContext(ctx,
			`INSERT INTO charging_orders (order_no, user_id, station_id, charger_id, status,
			                              energy_wh, amount_cents, stopped_at, completed_at)
			 VALUES ($1, $2, $3, $4, $5, $6, $7,
			         CASE WHEN $8::bigint IS NULL THEN NULL ELSE to_timestamp($8::bigint) END,
			         CASE WHEN $8::bigint IS NULL THEN NULL ELSE to_timestamp($8::bigint) END)`,
			order.orderNo, userID, stationID, chargerIDs[order.chargerIndex], order.status,
			order.energyWh, order.amountCent, stoppedAt); err != nil {
			t.Fatalf("insert order %s: %v", order.orderNo, err)
		}
	}

	cleanup := func() {
		_, _ = db.ExecContext(ctx, `DELETE FROM charging_orders WHERE station_id = $1`, stationID)
		_, _ = db.ExecContext(ctx, `DELETE FROM chargers WHERE station_id = $1`, stationID)
		_, _ = db.ExecContext(ctx, `DELETE FROM stations WHERE id = $1`, stationID)
		_, _ = db.ExecContext(ctx, `DELETE FROM user_accounts WHERE id = $1`, userID)
	}
	return stationID, userID, windowStart, cleanup
}

func TestStatsRevenueBucketsAndTotals(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewAdminStore(db)
	if err != nil {
		t.Fatalf("NewAdminStore() error = %v", err)
	}
	stationID, _, windowStart, cleanup := seedStatsFixture(t, db, ctx)
	defer cleanup()

	query := admin.RevenueQuery{
		FromAt:    windowStart,
		ToAt:      windowStart + 4*3600,
		StationID: stationID,
		Bucket:    admin.StatsBucketHour,
	}

	// Three completed orders, in the first, second and fourth hour. The
	// cancelled one is not revenue, whatever it was worth.
	totals, err := store.RevenueTotals(ctx, query)
	if err != nil {
		t.Fatalf("RevenueTotals() error = %v", err)
	}
	if totals.AmountCent != 3500 || totals.OrderCount != 3 {
		t.Fatalf("totals = %+v, want 3500 cents over 3 orders", totals)
	}
	// 2000 + 3000 + 500 Wh, reported in milliwatt-hours.
	if totals.EnergyMwh != 5_500_000 {
		t.Fatalf("energy = %d mWh, want 5500000", totals.EnergyMwh)
	}

	buckets, err := store.RevenueBuckets(ctx, query)
	if err != nil {
		t.Fatalf("RevenueBuckets() error = %v", err)
	}
	if len(buckets) != 3 {
		t.Fatalf("buckets = %+v, want the three hours that had revenue", buckets)
	}
	want := []admin.RevenueBucket{
		{BucketStart: windowStart, AmountCent: 1000, EnergyMwh: 2_000_000, OrderCount: 1},
		{BucketStart: windowStart + 3600, AmountCent: 2000, EnergyMwh: 3_000_000, OrderCount: 1},
		{BucketStart: windowStart + 3*3600, AmountCent: 500, EnergyMwh: 500_000, OrderCount: 1},
	}
	for index, expected := range want {
		if buckets[index] != expected {
			t.Fatalf("bucket %d = %+v, want %+v", index, buckets[index], expected)
		}
	}
}

// Buckets follow the range the client asked for, not the epoch: a range that
// starts mid-hour must produce buckets that start mid-hour.
func TestStatsRevenueBucketsFollowTheRequestedStart(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewAdminStore(db)
	if err != nil {
		t.Fatalf("NewAdminStore() error = %v", err)
	}
	stationID, _, windowStart, cleanup := seedStatsFixture(t, db, ctx)
	defer cleanup()

	// Twenty minutes past the first order's hour: the first bucket holds only
	// that order's hour remainder, and the boundary is the requested start.
	fromAt := windowStart + 1200
	buckets, err := store.RevenueBuckets(ctx, admin.RevenueQuery{
		FromAt:    fromAt,
		ToAt:      windowStart + 4*3600,
		StationID: stationID,
		Bucket:    admin.StatsBucketHour,
	})
	if err != nil {
		t.Fatalf("RevenueBuckets() error = %v", err)
	}
	if len(buckets) == 0 {
		t.Fatal("no buckets came back")
	}
	if buckets[0].BucketStart != fromAt {
		t.Fatalf("first bucket starts at %d, want the requested %d", buckets[0].BucketStart, fromAt)
	}
	// The order at windowStart+0 is before the window and must be excluded; the
	// order at +1h and +3h are inside.
	total := int64(0)
	for _, bucket := range buckets {
		total += bucket.AmountCent
	}
	if total != 2500 {
		t.Fatalf("revenue inside the shifted window = %d, want 2500", total)
	}
}

// A day bucket spans twenty-four hours and is still aligned to the range.
func TestStatsRevenueDayBucket(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewAdminStore(db)
	if err != nil {
		t.Fatalf("NewAdminStore() error = %v", err)
	}
	stationID, _, windowStart, cleanup := seedStatsFixture(t, db, ctx)
	defer cleanup()

	buckets, err := store.RevenueBuckets(ctx, admin.RevenueQuery{
		FromAt:    windowStart,
		ToAt:      windowStart + 24*3600,
		StationID: stationID,
		Bucket:    admin.StatsBucketDay,
	})
	if err != nil {
		t.Fatalf("RevenueBuckets() error = %v", err)
	}
	if len(buckets) != 1 {
		t.Fatalf("buckets = %+v, want one day", buckets)
	}
	if buckets[0].BucketStart != windowStart || buckets[0].AmountCent != 3500 || buckets[0].OrderCount != 3 {
		t.Fatalf("day bucket = %+v", buckets[0])
	}
}

// The fleet filter is what makes the station selector work.
func TestStatsRevenueRespectsTheStationFilter(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewAdminStore(db)
	if err != nil {
		t.Fatalf("NewAdminStore() error = %v", err)
	}
	stationID, _, windowStart, cleanup := seedStatsFixture(t, db, ctx)
	defer cleanup()

	query := admin.RevenueQuery{
		FromAt: windowStart, ToAt: windowStart + 4*3600, Bucket: admin.StatsBucketHour,
	}

	mine, err := store.RevenueTotals(ctx, admin.RevenueQuery{
		FromAt: query.FromAt, ToAt: query.ToAt, StationID: stationID, Bucket: query.Bucket})
	if err != nil {
		t.Fatalf("RevenueTotals(station) error = %v", err)
	}
	// Another station that does not exist yields nothing rather than everything.
	other, err := store.RevenueTotals(ctx, admin.RevenueQuery{
		FromAt: query.FromAt, ToAt: query.ToAt, StationID: stationID + 1_000_000, Bucket: query.Bucket})
	if err != nil {
		t.Fatalf("RevenueTotals(other) error = %v", err)
	}
	if mine.OrderCount != 3 {
		t.Fatalf("filtered totals = %+v", mine)
	}
	if other.OrderCount != 0 || other.AmountCent != 0 {
		t.Fatalf("an unknown station reported %+v", other)
	}

	// With no filter the same orders are still there, and so is any other row in
	// this test database.
	all, err := store.RevenueTotals(ctx, admin.RevenueQuery{
		FromAt: query.FromAt, ToAt: query.ToAt, Bucket: query.Bucket})
	if err != nil {
		t.Fatalf("RevenueTotals(all) error = %v", err)
	}
	if all.OrderCount < mine.OrderCount {
		t.Fatalf("unfiltered %+v is smaller than filtered %+v", all, mine)
	}
}

func TestStatsChargerCounts(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewAdminStore(db)
	if err != nil {
		t.Fatalf("NewAdminStore() error = %v", err)
	}
	stationID, _, _, cleanup := seedStatsFixture(t, db, ctx)
	defer cleanup()

	counts, err := store.ChargerCounts(ctx, stationID)
	if err != nil {
		t.Fatalf("ChargerCounts() error = %v", err)
	}
	if counts.Idle != 1 || counts.Occupied != 1 || counts.Faulty != 1 || counts.Disabled != 1 {
		t.Fatalf("census = %+v", counts)
	}
	if counts.Restarting != 0 || counts.Total != 4 {
		t.Fatalf("census = %+v, want four chargers and none restarting", counts)
	}

	// The fleet-wide census includes this station's devices and every other one.
	all, err := store.ChargerCounts(ctx, 0)
	if err != nil {
		t.Fatalf("ChargerCounts(0) error = %v", err)
	}
	if all.Total < counts.Total {
		t.Fatalf("fleet total %d is smaller than one station's %d", all.Total, counts.Total)
	}
}

// The fleet census counts lifetime figures, and counts each of them once: the
// classic way to get this wrong is to join the order aggregates together and
// multiply them.
func TestStatsFleetCounts(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewAdminStore(db)
	if err != nil {
		t.Fatalf("NewAdminStore() error = %v", err)
	}
	stationID, _, _, cleanup := seedStatsFixture(t, db, ctx)
	defer cleanup()

	counts, err := store.FleetCounts(ctx)
	if err != nil {
		t.Fatalf("FleetCounts() error = %v", err)
	}

	// The completed/DC/AC counts are lifetime, so this station's three completed
	// orders (two on DC, one on AC) are inside them.
	if counts.CompletedOrders < 3 {
		t.Fatalf("completed orders = %d, want at least the three this fixture added", counts.CompletedOrders)
	}
	if counts.FastOrders+counts.SlowOrders != counts.CompletedOrders {
		t.Fatalf("the charger mix %d+%d does not add up to %d completed orders",
			counts.FastOrders, counts.SlowOrders, counts.CompletedOrders)
	}
	if counts.RegisteredUsers < 1 || counts.Stations < 1 {
		t.Fatalf("census = %+v, want at least the fixture's account and station", counts)
	}
	if counts.TotalRevenueCent < 3500 {
		t.Fatalf("lifetime revenue = %d, want at least this fixture's 3500", counts.TotalRevenueCent)
	}

	// A cancelled order is not revenue and not a completed charge.
	var cancelled int64
	if err := db.QueryRowContext(ctx,
		`SELECT COALESCE(SUM(amount_cents), 0) FROM charging_orders WHERE station_id = $1 AND status = 'CANCELLED'`,
		stationID).Scan(&cancelled); err != nil {
		t.Fatalf("cancelled sum: %v", err)
	}
	if cancelled == 0 {
		t.Fatal("the fixture's cancelled order is missing, so this test proves nothing")
	}
}
