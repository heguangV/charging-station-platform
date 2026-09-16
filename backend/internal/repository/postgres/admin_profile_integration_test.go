package postgres

import (
	"context"
	"database/sql"
	"encoding/json"
	"testing"

	"github.com/heguangV/charging-station-platform/backend/internal/admin"
)

// Station editing and the fleet-wide tariff against a real PostgreSQL.
//
// The audit rows are asserted from the table rather than from the store's
// return value: the point of an audit trail is that it is written by the same
// transaction that made the change, and a test that trusted the return value
// would pass even if the trail were never written.

// seedEditStation creates a station owned by this test and returns its id and a
// cleanup function.
func seedEditStation(t *testing.T, db *sql.DB, ctx context.Context) (int64, func()) {
	t.Helper()
	suffix := uniqueSuffix(t)
	var stationID int64
	if err := db.QueryRowContext(ctx,
		`INSERT INTO stations (code, name, address, latitude, longitude, status)
		 VALUES ($1, $2, $3, 30.5452, 104.0708, 'OPEN') RETURNING id`,
		"ST-EDIT-"+suffix, "原名"+suffix, "原地址").Scan(&stationID); err != nil {
		t.Fatalf("insert station: %v", err)
	}
	return stationID, func() {
		_, _ = db.ExecContext(ctx, `DELETE FROM operation_logs WHERE resource_type = 'station' AND resource_id = $1`,
			strconvFormatInt64(stationID))
		_, _ = db.ExecContext(ctx, `DELETE FROM stations WHERE id = $1`, stationID)
	}
}

func TestAdminUpdateStationWritesTheProfileAndAnAuditRow(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewAdminStore(db)
	if err != nil {
		t.Fatalf("NewAdminStore() error = %v", err)
	}
	stationID, cleanup := seedEditStation(t, db, ctx)
	defer cleanup()

	updated, err := store.UpdateStation(ctx, admin.UpdateStationCommand{
		AdminID: 11, StationID: stationID,
		Name: "新名字", Address: "新地址",
		LatitudeE6: 31_123456, LongitudeE6: 105_654321,
		TraceID: "trace-station-edit",
	})
	if err != nil {
		t.Fatalf("UpdateStation() error = %v", err)
	}
	if updated.Name != "新名字" || updated.Address != "新地址" {
		t.Fatalf("updated = %+v", updated)
	}
	// The column holds degrees; the contract carries E6, and the round trip has
	// to be exact or a station would drift a little on every edit.
	if updated.LatitudeE6 != 31_123456 || updated.LongitudeE6 != 105_654321 {
		t.Fatalf("coordinates = %d/%d, want the E6 values back", updated.LatitudeE6, updated.LongitudeE6)
	}
	if updated.Code == "" || updated.Status != "OPEN" {
		t.Fatalf("updated = %+v, want the code and status preserved", updated)
	}

	// And the row really holds them.
	var name, address string
	var latitude, longitude float64
	if err := db.QueryRowContext(ctx,
		`SELECT name, address, latitude::float8, longitude::float8 FROM stations WHERE id = $1`,
		stationID).Scan(&name, &address, &latitude, &longitude); err != nil {
		t.Fatalf("read back: %v", err)
	}
	if name != "新名字" || address != "新地址" {
		t.Fatalf("row = %q/%q", name, address)
	}
	if toE6(latitude) != 31_123456 || toE6(longitude) != 105_654321 {
		t.Fatalf("row coordinates = %f/%f", latitude, longitude)
	}

	// The audit row carries both sides of the change.
	var payload string
	var actorID, action, resourceType, resourceID, requestID string
	if err := db.QueryRowContext(ctx, `SELECT actor_id, action, resource_type, resource_id, request_id, payload::text
FROM operation_logs
WHERE resource_type = 'station' AND resource_id = $1 AND action = 'station.update'
ORDER BY id DESC LIMIT 1`, strconvFormatInt64(stationID)).
		Scan(&actorID, &action, &resourceType, &resourceID, &requestID, &payload); err != nil {
		t.Fatalf("read audit row: %v", err)
	}
	if actorID != "11" || action != "station.update" || requestID != "trace-station-edit" {
		t.Fatalf("audit = %s/%s/%s", actorID, action, requestID)
	}
	var decoded struct {
		Previous map[string]any `json:"previous"`
		New      map[string]any `json:"new"`
	}
	if err := json.Unmarshal([]byte(payload), &decoded); err != nil {
		t.Fatalf("audit payload is not JSON: %s", payload)
	}
	if decoded.Previous["name"] == decoded.New["name"] {
		t.Fatalf("the audit does not record a change: %s", payload)
	}
	if decoded.New["address"] != "新地址" {
		t.Fatalf("audit new = %#v", decoded.New)
	}
	// The code and the status are not part of this endpoint, so they must not
	// appear in its audit: a reader would otherwise believe they were editable.
	if _, present := decoded.New["code"]; present {
		t.Fatalf("the audit claims the code changed: %s", payload)
	}
	if _, present := decoded.New["status"]; present {
		t.Fatalf("the audit claims the status changed: %s", payload)
	}
}

func TestAdminUpdateStationReportsAMissingStation(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewAdminStore(db)
	if err != nil {
		t.Fatalf("NewAdminStore() error = %v", err)
	}
	_, err = store.UpdateStation(ctx, admin.UpdateStationCommand{
		AdminID: 1, StationID: 1 << 40, Name: "站", Address: "地址",
	})
	if err != admin.ErrStationNotFound {
		t.Fatalf("error = %v, want ErrStationNotFound", err)
	}
}

// seedTariffFleet creates chargers with the tariffs the test needs and returns
// their station id plus a cleanup.
func seedTariffFleet(t *testing.T, db *sql.DB, ctx context.Context, tariffs []struct {
	electricity int64
	service     int64
	offPeak     *int64
	start       *int16
	end         *int16
}) (int64, func()) {
	t.Helper()
	suffix := uniqueSuffix(t)

	var stationID int64
	if err := db.QueryRowContext(ctx,
		`INSERT INTO stations (code, name, address, status) VALUES ($1, '费率站', '地址', 'OPEN') RETURNING id`,
		"ST-TARIFF-"+suffix).Scan(&stationID); err != nil {
		t.Fatalf("insert station: %v", err)
	}
	ids := make([]int64, 0, len(tariffs))
	for index, tariff := range tariffs {
		var id int64
		if err := db.QueryRowContext(ctx, `INSERT INTO chargers
    (station_id, code, connector_type, power_watt, status,
     price_per_kwh_cents, service_price_per_kwh_cents,
     off_peak_electricity_price_per_kwh_cents, off_peak_start_hour, off_peak_end_hour)
VALUES ($1, $2, 'DC', 60000, 'IDLE', $3, $4, $5, $6, $7) RETURNING id`,
			stationID, "T"+suffix+string(rune('A'+index)),
			tariff.electricity, tariff.service, tariff.offPeak, tariff.start, tariff.end,
		).Scan(&id); err != nil {
			t.Fatalf("insert charger: %v", err)
		}
		ids = append(ids, id)
	}
	return stationID, func() {
		_, _ = db.ExecContext(ctx, `DELETE FROM operation_logs WHERE resource_type = 'tariff' AND resource_id = 'GLOBAL'`)
		_, _ = db.ExecContext(ctx, `DELETE FROM chargers WHERE station_id = $1`, stationID)
		_, _ = db.ExecContext(ctx, `DELETE FROM stations WHERE id = $1`, stationID)
	}
}

func TestAdminGlobalTariffGroupsTheFleet(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewAdminStore(db)
	if err != nil {
		t.Fatalf("NewAdminStore() error = %v", err)
	}
	offPeak := int64(60)
	start := int16(23)
	end := int16(7)
	_, cleanup := seedTariffFleet(t, db, ctx, []struct {
		electricity int64
		service     int64
		offPeak     *int64
		start       *int16
		end         *int16
	}{
		{electricity: 111, service: 51},
		{electricity: 111, service: 51},
		{electricity: 222, service: 52, offPeak: &offPeak, start: &start, end: &end},
	})
	defer cleanup()

	configurations, err := store.GlobalTariff(ctx)
	if err != nil {
		t.Fatalf("GlobalTariff() error = %v", err)
	}

	// The database holds other chargers from other tests; what is asserted is
	// that this test's three are grouped the way they were inserted.
	var flat, timeOfUse int64
	found := map[int64]int64{}
	for _, configuration := range configurations {
		found[configuration.ElectricityPriceCent] += configuration.ChargerCount
		if configuration.OffPeakPriceCent == nil {
			flat += configuration.ChargerCount
		}
	}
	if found[111] < 2 || found[222] < 1 {
		t.Fatalf("the two configurations were not grouped: %+v", configurations)
	}
	if flat < 2 {
		t.Fatalf("flat chargers = %d, want at least this test's two", flat)
	}
	_ = timeOfUse

	// The time-of-use configuration is reported with its window, which is what
	// makes a rate auditable.
	var seen bool
	for _, configuration := range configurations {
		if configuration.ElectricityPriceCent == 222 {
			if configuration.OffPeakPriceCent == nil || *configuration.OffPeakPriceCent != 60 ||
				configuration.OffPeakStartHour == nil || *configuration.OffPeakStartHour != 23 {
				t.Fatalf("the time-of-use configuration lost its window: %+v", configuration)
			}
			seen = true
		}
	}
	if !seen {
		t.Fatal("the time-of-use configuration was not reported")
	}

	// A station identifier is deliberately not accepted by this view: it is the
	// fleet's tariff, and a per-station view would be a second way to ask a
	// question the charger list already answers.
}

func TestAdminUpdateGlobalTariffWritesEveryChargerAndAudits(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewAdminStore(db)
	if err != nil {
		t.Fatalf("NewAdminStore() error = %v", err)
	}
	_, cleanup := seedTariffFleet(t, db, ctx, []struct {
		electricity int64
		service     int64
		offPeak     *int64
		start       *int16
		end         *int16
	}{
		{electricity: 111, service: 51},
		{electricity: 222, service: 52},
	})
	defer cleanup()

	// Every charger in the database is in scope, so the expected count is read
	// from the database rather than assumed to be this test's two.
	var total int64
	if err := db.QueryRowContext(ctx, `SELECT count(*) FROM chargers`).Scan(&total); err != nil {
		t.Fatalf("count chargers: %v", err)
	}

	offPeak := int64(70)
	start := int16(22)
	end := int16(6)
	result, err := store.UpdateGlobalTariff(ctx, admin.GlobalTariffUpdate{
		AdminID: 12, ElectricityPriceCent: 100, ServicePriceCent: 55,
		OffPeakPriceCent: &offPeak, OffPeakStartHour: &start, OffPeakEndHour: &end,
		Reason: "统一峰谷费率", TraceID: "trace-tariff",
	})
	if err != nil {
		t.Fatalf("UpdateGlobalTariff() error = %v", err)
	}
	if result.AffectedChargers != total {
		t.Fatalf("affected = %d, want every charger (%d)", result.AffectedChargers, total)
	}
	if result.PreviousConfigurations < 2 {
		t.Fatalf("previous configurations = %d, want at least this test's two",
			result.PreviousConfigurations)
	}

	// No charger was left behind, including the ones this test did not create.
	var stragglers int64
	if err := db.QueryRowContext(ctx, `SELECT count(*) FROM chargers
WHERE price_per_kwh_cents <> 100 OR service_price_per_kwh_cents <> 55
   OR off_peak_electricity_price_per_kwh_cents IS DISTINCT FROM 70
   OR off_peak_start_hour IS DISTINCT FROM 22
   OR off_peak_end_hour IS DISTINCT FROM 6`).Scan(&stragglers); err != nil {
		t.Fatalf("count stragglers: %v", err)
	}
	if stragglers != 0 {
		t.Fatalf("%d chargers do not carry the fleet tariff", stragglers)
	}

	var payload string
	var actorID, requestID string
	if err := db.QueryRowContext(ctx, `SELECT actor_id, request_id, payload::text
FROM operation_logs
WHERE resource_type = 'tariff' AND resource_id = 'GLOBAL' AND action = 'tariff.global-update'
ORDER BY id DESC LIMIT 1`).Scan(&actorID, &requestID, &payload); err != nil {
		t.Fatalf("read audit row: %v", err)
	}
	if actorID != "12" || requestID != "trace-tariff" {
		t.Fatalf("audit = %s/%s", actorID, requestID)
	}
	var decoded map[string]any
	if err := json.Unmarshal([]byte(payload), &decoded); err != nil {
		t.Fatalf("audit payload is not JSON: %s", payload)
	}
	if decoded["reason"] != "统一峰谷费率" {
		t.Fatalf("audit reason = %#v", decoded["reason"])
	}
	if decoded["affectedChargers"] != float64(total) {
		t.Fatalf("audit affected = %#v, want %d", decoded["affectedChargers"], total)
	}
	applied, _ := decoded["new"].(map[string]any)
	if applied == nil || applied["electricityPrice"] != float64(100) || applied["offPeakPrice"] != float64(70) {
		t.Fatalf("audit new = %#v", decoded["new"])
	}
}

// Clearing the time-of-use window is a real change and has to be recorded as
// one: a fleet that silently loses its off-peak price is a billing defect.
func TestAdminUpdateGlobalTariffClearsTheOffPeakWindow(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewAdminStore(db)
	if err != nil {
		t.Fatalf("NewAdminStore() error = %v", err)
	}
	offPeak := int64(60)
	start := int16(23)
	end := int16(7)
	stationID, cleanup := seedTariffFleet(t, db, ctx, []struct {
		electricity int64
		service     int64
		offPeak     *int64
		start       *int16
		end         *int16
	}{
		{electricity: 333, service: 53, offPeak: &offPeak, start: &start, end: &end},
	})
	defer cleanup()

	if _, err := store.UpdateGlobalTariff(ctx, admin.GlobalTariffUpdate{
		AdminID: 1, ElectricityPriceCent: 90, ServicePriceCent: 40, Reason: "改为单一费率",
	}); err != nil {
		t.Fatalf("UpdateGlobalTariff() error = %v", err)
	}

	var withWindow int64
	if err := db.QueryRowContext(ctx, `SELECT count(*) FROM chargers
WHERE station_id = $1 AND off_peak_electricity_price_per_kwh_cents IS NOT NULL`, stationID).Scan(&withWindow); err != nil {
		t.Fatalf("count: %v", err)
	}
	if withWindow != 0 {
		t.Fatalf("%d chargers kept their off-peak window after a flat fleet tariff", withWindow)
	}
}
