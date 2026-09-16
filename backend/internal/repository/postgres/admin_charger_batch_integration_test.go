package postgres

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"testing"

	"github.com/heguangV/charging-station-platform/backend/internal/admin"
	"github.com/heguangV/charging-station-platform/backend/internal/order"
)

// Batch device creation against a real PostgreSQL.
//
// The unit tests prove what the endpoint accepts. What can only be proved here
// is the property the feature exists for: a batch either creates every device it
// was given or creates none of them, and a retry creates nothing twice.

// seedBatchStation creates a station for a batch test and returns its id with a
// cleanup that removes everything the test created.
func seedBatchStation(t *testing.T, db *sql.DB, ctx context.Context) (int64, string, func()) {
	t.Helper()
	suffix := uniqueSuffix(t)
	var stationID int64
	if err := db.QueryRowContext(ctx,
		`INSERT INTO stations (code, name, address, status) VALUES ($1, $2, $3, 'OPEN') RETURNING id`,
		"ST-BATCH-"+suffix, "批量建桩站"+suffix, "地址").Scan(&stationID); err != nil {
		t.Fatalf("insert station: %v", err)
	}
	return stationID, suffix, func() {
		_, _ = db.ExecContext(ctx, `DELETE FROM operation_logs WHERE resource_type = 'station' AND resource_id = $1`,
			strconvFormatInt64(stationID))
		_, _ = db.ExecContext(ctx, `DELETE FROM idempotency_records WHERE scope LIKE 'admin:%:charger:batch'`)
		_, _ = db.ExecContext(ctx, `DELETE FROM chargers WHERE station_id = $1`, stationID)
		_, _ = db.ExecContext(ctx, `DELETE FROM stations WHERE id = $1`, stationID)
	}
}

func batchCodes(t *testing.T, db *sql.DB, ctx context.Context, stationID int64) []string {
	t.Helper()
	rows, err := db.QueryContext(ctx, `SELECT code FROM chargers WHERE station_id = $1 ORDER BY code`, stationID)
	if err != nil {
		t.Fatalf("read chargers: %v", err)
	}
	defer rows.Close()
	codes := []string{}
	for rows.Next() {
		var code string
		if err := rows.Scan(&code); err != nil {
			t.Fatalf("scan code: %v", err)
		}
		codes = append(codes, code)
	}
	if err := rows.Err(); err != nil {
		t.Fatalf("rows: %v", err)
	}
	return codes
}

func TestAdminCreateChargersBatchCreatesEveryDeviceAndAudits(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewAdminStore(db)
	if err != nil {
		t.Fatalf("NewAdminStore() error = %v", err)
	}
	stationID, suffix, cleanup := seedBatchStation(t, db, ctx)
	defer cleanup()

	result, err := store.CreateChargers(ctx, admin.CreateChargersCommand{
		AdminID: 21, StationID: stationID,
		Chargers: []admin.ChargerDraft{
			{Code: "B-" + suffix + "-01", ConnectorType: "DC", PowerWatt: 120000},
			{Code: "B-" + suffix + "-02", ConnectorType: "DC", PowerWatt: 120000},
			{Code: "B-" + suffix + "-03", ConnectorType: "AC", PowerWatt: 7000},
		},
		IdempotencyKey: "batch-key-" + suffix, RequestHash: "hash-1", TraceID: "trace-batch",
	})
	if err != nil {
		t.Fatalf("CreateChargers() error = %v", err)
	}
	if result.ChargerCount != 3 || len(result.Created) != 3 {
		t.Fatalf("result = %+v, want three devices", result)
	}
	for _, record := range result.Created {
		if record.ID == 0 || record.Status != "IDLE" || record.StationID != stationID {
			t.Fatalf("record = %+v, want a created device at this station in IDLE", record)
		}
	}

	codes := batchCodes(t, db, ctx, stationID)
	if len(codes) != 3 {
		t.Fatalf("codes = %v, want three rows", codes)
	}

	// The audit records who, where, how many, and which codes: the question an
	// operator asks when the count disagrees with the cabinet.
	var actorID, requestID, payload string
	if err := db.QueryRowContext(ctx, `SELECT actor_id, request_id, payload::text
FROM operation_logs
WHERE resource_type = 'station' AND resource_id = $1 AND action = 'charger.batch-create'
ORDER BY id DESC LIMIT 1`, strconvFormatInt64(stationID)).Scan(&actorID, &requestID, &payload); err != nil {
		t.Fatalf("read audit row: %v", err)
	}
	if actorID != "21" || requestID != "trace-batch" {
		t.Fatalf("audit = %s/%s", actorID, requestID)
	}
	var decoded struct {
		ChargerCount int64    `json:"chargerCount"`
		Codes        []string `json:"codes"`
	}
	if err := json.Unmarshal([]byte(payload), &decoded); err != nil {
		t.Fatalf("audit payload is not JSON: %s", payload)
	}
	if decoded.ChargerCount != 3 || len(decoded.Codes) != 3 {
		t.Fatalf("audit = %+v, want the count and the codes", decoded)
	}
}

// The property the feature exists for: a code that already exists takes the
// whole batch with it, and nothing is left half-created.
func TestAdminCreateChargersBatchCreatesNothingWhenOneCodeExists(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewAdminStore(db)
	if err != nil {
		t.Fatalf("NewAdminStore() error = %v", err)
	}
	stationID, suffix, cleanup := seedBatchStation(t, db, ctx)
	defer cleanup()

	// One device already at the station.
	existing := "B-" + suffix + "-02"
	if _, err := db.ExecContext(ctx,
		`INSERT INTO chargers (station_id, code, connector_type, power_watt, status)
		 VALUES ($1, $2, 'DC', 120000, 'IDLE')`, stationID, existing); err != nil {
		t.Fatalf("insert existing charger: %v", err)
	}

	_, err = store.CreateChargers(ctx, admin.CreateChargersCommand{
		AdminID: 21, StationID: stationID,
		Chargers: []admin.ChargerDraft{
			{Code: "B-" + suffix + "-01", ConnectorType: "DC", PowerWatt: 120000},
			{Code: existing, ConnectorType: "DC", PowerWatt: 120000},
			{Code: "B-" + suffix + "-03", ConnectorType: "DC", PowerWatt: 120000},
		},
		IdempotencyKey: "batch-conflict-" + suffix, RequestHash: "hash-2",
	})
	if !errors.Is(err, admin.ErrDuplicateChargerCode) {
		t.Fatalf("error = %v, want ErrDuplicateChargerCode", err)
	}

	// The decisive assertion: the two codes that did not collide were not created
	// either.
	codes := batchCodes(t, db, ctx, stationID)
	if len(codes) != 1 || codes[0] != existing {
		t.Fatalf("codes = %v, want only the device that already existed", codes)
	}
}

// A draft the database itself refuses - here a connector type outside the CHECK
// constraint, which the service would have caught - still aborts the whole
// batch, so a constraint is a second line of defence rather than a partial write.
func TestAdminCreateChargersBatchRollsBackOnAConstraintViolation(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewAdminStore(db)
	if err != nil {
		t.Fatalf("NewAdminStore() error = %v", err)
	}
	stationID, suffix, cleanup := seedBatchStation(t, db, ctx)
	defer cleanup()

	_, err = store.CreateChargers(ctx, admin.CreateChargersCommand{
		AdminID: 21, StationID: stationID,
		Chargers: []admin.ChargerDraft{
			{Code: "B-" + suffix + "-01", ConnectorType: "DC", PowerWatt: 120000},
			{Code: "B-" + suffix + "-02", ConnectorType: "HVDC", PowerWatt: 120000},
		},
		IdempotencyKey: "batch-constraint-" + suffix, RequestHash: "hash-3",
	})
	if err == nil {
		t.Fatal("expected the constraint violation to be reported")
	}
	if codes := batchCodes(t, db, ctx, stationID); len(codes) != 0 {
		t.Fatalf("codes = %v, want nothing created", codes)
	}
}

// The same key with the same body is the same request: it reports the first
// result and creates nothing twice.
func TestAdminCreateChargersBatchIsIdempotent(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewAdminStore(db)
	if err != nil {
		t.Fatalf("NewAdminStore() error = %v", err)
	}
	stationID, suffix, cleanup := seedBatchStation(t, db, ctx)
	defer cleanup()

	command := admin.CreateChargersCommand{
		AdminID: 21, StationID: stationID,
		Chargers: []admin.ChargerDraft{
			{Code: "B-" + suffix + "-01", ConnectorType: "DC", PowerWatt: 120000},
			{Code: "B-" + suffix + "-02", ConnectorType: "DC", PowerWatt: 120000},
		},
		IdempotencyKey: "batch-retry-" + suffix, RequestHash: "hash-4", TraceID: "trace-batch",
	}

	first, err := store.CreateChargers(ctx, command)
	if err != nil {
		t.Fatalf("first CreateChargers() error = %v", err)
	}
	replay, err := store.CreateChargers(ctx, command)
	if err != nil {
		t.Fatalf("replayed CreateChargers() error = %v", err)
	}
	if replay.ChargerCount != first.ChargerCount || len(replay.Created) != len(first.Created) {
		t.Fatalf("replay = %+v, want the first result %+v", replay, first)
	}
	if replay.Created[0].ID != first.Created[0].ID {
		t.Fatalf("replay returned different devices: %+v vs %+v", replay.Created[0], first.Created[0])
	}
	if codes := batchCodes(t, db, ctx, stationID); len(codes) != 2 {
		t.Fatalf("codes = %v, want the two devices created once", codes)
	}

	// The same key with a different body is a different request under a used key.
	changed := command
	changed.RequestHash = "hash-5"
	changed.Chargers = append(changed.Chargers, admin.ChargerDraft{
		Code: "B-" + suffix + "-03", ConnectorType: "AC", PowerWatt: 7000,
	})
	if _, err := store.CreateChargers(ctx, changed); !errors.Is(err, order.ErrIdempotencyConflict) {
		t.Fatalf("error = %v, want ErrIdempotencyConflict", err)
	}
	if codes := batchCodes(t, db, ctx, stationID); len(codes) != 2 {
		t.Fatalf("codes = %v, want the conflict to have changed nothing", codes)
	}
}

func TestAdminCreateChargersBatchReportsAMissingStation(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewAdminStore(db)
	if err != nil {
		t.Fatalf("NewAdminStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	_, err = store.CreateChargers(ctx, admin.CreateChargersCommand{
		AdminID: 21, StationID: 1 << 40,
		Chargers:       []admin.ChargerDraft{{Code: "B-" + suffix, ConnectorType: "DC", PowerWatt: 120000}},
		IdempotencyKey: "batch-missing-" + suffix, RequestHash: "hash-6",
	})
	if !errors.Is(err, admin.ErrStationNotFound) {
		t.Fatalf("error = %v, want ErrStationNotFound", err)
	}
}
