package postgres

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"io/fs"
	"os"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	testingfs "testing/fstest"
	"time"

	"github.com/heguangV/charging-station-platform/backend/internal/auth"
	"github.com/heguangV/charging-station-platform/backend/internal/station"
	"github.com/heguangV/charging-station-platform/backend/migrations"
)

// testDatabaseLockKey namespaces the advisory lock that serializes test
// binaries sharing one database ("NCS_TEST").
const testDatabaseLockKey int64 = 0x4E43535F54455354

// lockTestDatabase takes a session-level advisory lock for the duration of one
// test.
//
// TestRunSerializesConcurrentRuns and TestRunWorksWithSingleConnectionPool drop
// and rebuild every table, so two test binaries pointed at the same database
// destroy each other's schema: with a second process running, an unrelated test
// failed with `relation "schema_migrations" does not exist` (SQLSTATE 42P01).
// The lock makes the schema-rebuilding tests exclusive across processes instead
// of documenting a rule nobody can enforce.
func lockTestDatabase(t *testing.T, dsn string) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Minute)
	t.Cleanup(cancel)

	lockDB, err := Open(ctx, dsn, 1)
	if err != nil {
		t.Fatalf("open the lock connection: %v", err)
	}
	conn, err := lockDB.Conn(ctx)
	if err != nil {
		_ = lockDB.Close()
		t.Fatalf("take the lock connection: %v", err)
	}
	if _, err := conn.ExecContext(ctx, `SELECT pg_advisory_lock($1)`, testDatabaseLockKey); err != nil {
		_ = conn.Close()
		_ = lockDB.Close()
		t.Fatalf("lock the test database: %v", err)
	}
	t.Cleanup(func() {
		// Closing the connection releases the lock even if the unlock fails.
		_, _ = conn.ExecContext(context.Background(), `SELECT pg_advisory_unlock($1)`, testDatabaseLockKey)
		_ = conn.Close()
		_ = lockDB.Close()
	})
}

// integrationDB returns a migrated database when NCS_TEST_PG_DSN points at a
// disposable database; tests skip otherwise. The schema is applied through
// the same runner the API uses at startup.
func integrationDB(t *testing.T) (*sql.DB, context.Context) {
	t.Helper()
	dsn := os.Getenv("NCS_TEST_PG_DSN")
	if dsn == "" {
		t.Skip("NCS_TEST_PG_DSN not set; PostgreSQL integration tests skipped")
	}
	lockTestDatabase(t, dsn)

	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	t.Cleanup(cancel)

	db, err := Open(ctx, dsn, 4)
	if err != nil {
		t.Fatalf("Open() error = %v", err)
	}
	t.Cleanup(func() { _ = db.Close() })

	runnerDB, err := NewSQLDB(db)
	if err != nil {
		t.Fatalf("NewSQLDB() error = %v", err)
	}
	report, err := Run(ctx, runnerDB, migrations.FS)
	if err != nil {
		t.Fatalf("Run() migrations error = %v", err)
	}
	_ = report
	return db, ctx
}

var fixtureSequence atomic.Uint64

func uniqueSuffix(t *testing.T) string {
	t.Helper()
	// Callers truncate this suffix for phone fixtures: put the counter first,
	// so fixtures created in the same centisecond cannot collide.
	return fmt.Sprintf("%08d%d", fixtureSequence.Add(1), time.Now().UnixNano())
}

func TestAccountStoreRoundTrip(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewAccountStore(db)
	if err != nil {
		t.Fatalf("NewAccountStore() error = %v", err)
	}

	hash, err := auth.HashPasswordWithIterations("Dev-Password-01", 1000)
	if err != nil {
		t.Fatalf("hash: %v", err)
	}
	phone := "139" + uniqueSuffix(t)[:9]
	email := phone + "@dev.local"

	if _, err := db.ExecContext(ctx,
		`INSERT INTO user_accounts (phone, email, display_name, password_hash) VALUES ($1, $2, $3, $4)`,
		phone, email, "集成测试用户", hash); err != nil {
		t.Fatalf("seed user: %v", err)
	}

	byPhone, err := store.FindUserByAccount(ctx, phone)
	if err != nil {
		t.Fatalf("FindUserByAccount(phone) error = %v", err)
	}
	if byPhone == nil || byPhone.Phone != phone || byPhone.DisplayName != "集成测试用户" {
		t.Fatalf("byPhone = %#v", byPhone)
	}
	byEmail, err := store.FindUserByAccount(ctx, email)
	if err != nil || byEmail == nil || byEmail.ID != byPhone.ID {
		t.Fatalf("FindUserByAccount(email) = %#v, %v", byEmail, err)
	}
	if unknown, err := store.FindUserByAccount(ctx, "10000000000"); err != nil || unknown != nil {
		t.Fatalf("unknown account = %#v, %v; want nil, nil", unknown, err)
	}

	username := "it_admin_" + uniqueSuffix(t)
	if _, err := db.ExecContext(ctx,
		`INSERT INTO admin_accounts (username, password_hash, role) VALUES ($1, $2, 'OPERATOR')`,
		username, hash); err != nil {
		t.Fatalf("seed admin: %v", err)
	}
	admin, err := store.FindAdminByUsername(ctx, username)
	if err != nil || admin == nil || admin.Username != username {
		t.Fatalf("FindAdminByUsername = %#v, %v", admin, err)
	}
}

func TestStationStoreQueries(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewStationStore(db)
	if err != nil {
		t.Fatalf("NewStationStore() error = %v", err)
	}

	suffix := uniqueSuffix(t)
	code := "IT-" + suffix
	var stationID int64
	if err := db.QueryRowContext(ctx,
		`INSERT INTO stations (code, name, address, latitude, longitude, status)
VALUES ($1, $2, '集成测试地址', 31.230400, 121.473700, 'OPEN') RETURNING id`,
		code, "集成测试站"+suffix).Scan(&stationID); err != nil {
		t.Fatalf("seed station: %v", err)
	}
	for _, charger := range []struct {
		code   string
		power  int64
		status string
	}{
		{"C01", 7000, station.ChargerStatusIdle},
		{"C02", 120000, station.ChargerStatusOccupied},
	} {
		if _, err := db.ExecContext(ctx,
			`INSERT INTO chargers (station_id, code, connector_type, power_watt, status) VALUES ($1, $2, 'DC', $3, $4)`,
			stationID, charger.code, charger.power, charger.status); err != nil {
			t.Fatalf("seed charger: %v", err)
		}
	}

	detail, err := store.GetStation(ctx, stationID)
	if err != nil {
		t.Fatalf("GetStation() error = %v", err)
	}
	if detail.Code != code || detail.ChargerCount != 2 || detail.LatitudeE6 != 31230400 || detail.LongitudeE6 != 121473700 {
		t.Fatalf("detail = %#v", detail)
	}
	if _, err := store.GetStation(ctx, -1); !errors.Is(err, station.ErrStationNotFound) {
		// Negative ids reach the store only through the service guard; the
		// store maps unknown ids to ErrStationNotFound.
		t.Fatalf("GetStation(-1) error = %v, want ErrStationNotFound", err)
	}

	page, err := store.ListStations(ctx, station.StationFilter{Page: 1, PageSize: 100, Keyword: code})
	if err != nil {
		t.Fatalf("ListStations() error = %v", err)
	}
	if len(page.Items) != 1 || page.Meta.Total != 1 || page.Items[0].ID != stationID {
		t.Fatalf("page = %#v", page)
	}

	empty, err := store.ListStations(ctx, station.StationFilter{Page: 1, PageSize: 100, Keyword: "no-such-keyword-" + suffix})
	if err != nil || len(empty.Items) != 0 {
		t.Fatalf("keyword filter matched unexpected rows: %#v, %v", empty.Items, err)
	}

	chargers, err := store.ListChargers(ctx, station.ChargerFilter{Page: 1, PageSize: 100, StationID: stationID, StationIDSet: true})
	if err != nil || len(chargers.Items) != 2 {
		t.Fatalf("chargers = %#v, %v", chargers, err)
	}
	idle, err := store.ListChargers(ctx, station.ChargerFilter{
		Page: 1, PageSize: 100, StationID: stationID, StationIDSet: true, Status: station.ChargerStatusIdle,
	})
	if err != nil || len(idle.Items) != 1 || idle.Items[0].Code != "C01" {
		t.Fatalf("idle chargers = %#v, %v", idle, err)
	}
}

func TestEndToEndLoginAgainstPostgreSQL(t *testing.T) {
	db, ctx := integrationDB(t)

	hash, err := auth.HashPasswordWithIterations("Dev-Password-01", 1000)
	if err != nil {
		t.Fatalf("hash: %v", err)
	}
	phone := "137" + uniqueSuffix(t)[:9]
	if _, err := db.ExecContext(ctx,
		`INSERT INTO user_accounts (phone, display_name, password_hash) VALUES ($1, $2, $3)`,
		phone, "端到端用户", hash); err != nil {
		t.Fatalf("seed user: %v", err)
	}

	service, err := auth.NewService(
		mustAccountStore(t, db),
		mustAccountStore(t, db),
		mustAccountMutation(t, db),
		auth.NewInMemorySessionStore(time.Minute, nil),
		auth.NewFixedWindowLimiter(100, time.Minute, nil),
		auth.NewInMemorySMSCodeStore(nil),
		time.Minute, time.Hour,
	)
	if err != nil {
		t.Fatalf("auth service: %v", err)
	}

	result, err := service.Login(ctx, phone, "Dev-Password-01")
	if err != nil {
		t.Fatalf("Login() error = %v", err)
	}
	identity, err := service.Identify(ctx, result.Token)
	if err != nil {
		t.Fatalf("Identify() error = %v", err)
	}
	if identity.Role != auth.RoleUser || identity.DisplayName != "端到端用户" {
		t.Fatalf("identity = %#v", identity)
	}
	if _, err := service.Login(ctx, phone, "Wrong-Password-1"); err == nil {
		t.Fatal("wrong password accepted")
	}
}

func mustAccountStore(t *testing.T, db *sql.DB) *AccountStore {
	t.Helper()
	store, err := NewAccountStore(db)
	if err != nil {
		t.Fatalf("account store: %v", err)
	}
	return store
}

func mustAccountMutation(t *testing.T, db *sql.DB) *AccountMutationAdapter {
	t.Helper()
	store, err := NewAccountMutationAdapter(db)
	if err != nil {
		t.Fatalf("account mutation adapter: %v", err)
	}
	return store
}

// orderFixture seeds one user, two stations and one charger in the first
// station, returning their ids.
func orderFixture(t *testing.T, db *sql.DB, ctx context.Context, suffix string) (userID, stationA, stationB, chargerA int64) {
	t.Helper()
	hash, err := auth.HashPasswordWithIterations("Dev-Password-01", 1000)
	if err != nil {
		t.Fatalf("hash: %v", err)
	}
	if err := db.QueryRowContext(ctx,
		`INSERT INTO user_accounts (phone, password_hash) VALUES ($1, $2) RETURNING id`,
		"136"+suffix[:9], hash).Scan(&userID); err != nil {
		t.Fatalf("seed user: %v", err)
	}
	if err := db.QueryRowContext(ctx,
		`INSERT INTO stations (code, name, status) VALUES ($1, '站A', 'OPEN') RETURNING id`,
		"IT-A-"+suffix).Scan(&stationA); err != nil {
		t.Fatalf("seed station A: %v", err)
	}
	if err := db.QueryRowContext(ctx,
		`INSERT INTO stations (code, name, status) VALUES ($1, '站B', 'OPEN') RETURNING id`,
		"IT-B-"+suffix).Scan(&stationB); err != nil {
		t.Fatalf("seed station B: %v", err)
	}
	if err := db.QueryRowContext(ctx,
		`INSERT INTO chargers (station_id, code, connector_type, power_watt, status) VALUES ($1, 'C01', 'DC', 120000, 'IDLE') RETURNING id`,
		stationA).Scan(&chargerA); err != nil {
		t.Fatalf("seed charger: %v", err)
	}
	return userID, stationA, stationB, chargerA
}

func TestOrderConstraintsRejectInconsistentData(t *testing.T) {
	db, ctx := integrationDB(t)
	suffix := uniqueSuffix(t)
	userID, stationA, stationB, chargerA := orderFixture(t, db, ctx, suffix)

	// A station-B order may not use a station-A charger: the composite FK
	// binds (charger_id, station_id) to the charger row.
	_, err := db.ExecContext(ctx,
		`INSERT INTO charging_orders (order_no, user_id, station_id, charger_id, status)
VALUES ($1, $2, $3, $4, 'CHARGING')`,
		"ORD-XS-"+suffix, userID, stationB, chargerA)
	if err == nil || !strings.Contains(err.Error(), "foreign key constraint") {
		t.Fatalf("cross-station order error = %v, want composite FK violation", err)
	}

	// The consistent pairing inserts cleanly.
	if _, err := db.ExecContext(ctx,
		`INSERT INTO charging_orders (order_no, user_id, station_id, charger_id, status)
VALUES ($1, $2, $3, $4, 'CHARGING')`,
		"ORD-OK-"+suffix, userID, stationA, chargerA); err != nil {
		t.Fatalf("consistent order rejected: %v", err)
	}

	// A second active order for the same charger is rejected by the partial
	// unique index, regardless of the status inside the active set.
	_, err = db.ExecContext(ctx,
		`INSERT INTO charging_orders (order_no, user_id, station_id, charger_id, status)
VALUES ($1, $2, $3, $4, 'STARTING')`,
		"ORD-ACT-"+suffix, userID, stationA, chargerA)
	if err == nil || !strings.Contains(err.Error(), "uq_orders_charger_active") {
		t.Fatalf("second active order error = %v, want uq_orders_charger_active violation", err)
	}

	// Duplicate order numbers stay rejected.
	_, err = db.ExecContext(ctx,
		`INSERT INTO charging_orders (order_no, user_id, station_id, charger_id, status)
VALUES ($1, $2, $3, $4, 'CHARGING')`,
		"ORD-OK-"+suffix, userID, stationA, chargerA)
	if err == nil || !strings.Contains(err.Error(), "charging_orders_order_no_key") {
		t.Fatalf("duplicate order_no error = %v, want unique violation", err)
	}

	// Once the active order completes, the charger is free again.
	if _, err := db.ExecContext(ctx,
		`UPDATE charging_orders SET status = 'COMPLETED' WHERE order_no = $1`,
		"ORD-OK-"+suffix); err != nil {
		t.Fatalf("complete order: %v", err)
	}
	if _, err := db.ExecContext(ctx,
		`INSERT INTO charging_orders (order_no, user_id, station_id, charger_id, status)
VALUES ($1, $2, $3, $4, 'CREATED')`,
		"ORD-NEXT-"+suffix, userID, stationA, chargerA); err != nil {
		t.Fatalf("order after completion rejected: %v", err)
	}
}

func TestConcurrentActiveOrderInsertsSingleWinner(t *testing.T) {
	db, ctx := integrationDB(t)
	suffix := uniqueSuffix(t)
	userID, stationA, _, chargerA := orderFixture(t, db, ctx, suffix)

	connA, err := db.Conn(ctx)
	if err != nil {
		t.Fatalf("conn A: %v", err)
	}
	defer connA.Close()
	connB, err := db.Conn(ctx)
	if err != nil {
		t.Fatalf("conn B: %v", err)
	}
	defer connB.Close()

	barrier := make(chan struct{})
	errs := make(chan error, 2)
	for worker, conn := range map[int]*sql.Conn{0: connA, 1: connB} {
		go func(worker int, conn *sql.Conn) {
			<-barrier
			_, err := conn.ExecContext(ctx,
				`INSERT INTO charging_orders (order_no, user_id, station_id, charger_id, status)
VALUES ($1, $2, $3, $4, 'CHARGING')`,
				"ORD-RACE-"+suffix+"-"+strconv.Itoa(worker), userID, stationA, chargerA)
			errs <- err
		}(worker, conn)
	}
	close(barrier)

	succeeded := 0
	raced := 0
	for received := 0; received < 2; received++ {
		err := <-errs
		switch {
		case err == nil:
			succeeded++
		case strings.Contains(err.Error(), "uq_orders_charger_active"):
			raced++
		default:
			t.Fatalf("unexpected insert error: %v", err)
		}
	}
	if succeeded != 1 || raced != 1 {
		t.Fatalf("race outcome = %d succeeded, %d rejected; want exactly one winner", succeeded, raced)
	}
}

func TestRunSerializesConcurrentRuns(t *testing.T) {
	db, ctx := integrationDB(t)

	// Reset the schema so the concurrent runs actually race to apply 0001.
	for _, table := range []string{
		"schema_migrations", "order_events", "charging_orders", "wallet_transactions",
		"wallet_accounts", "operation_logs", "idempotency_records", "outbox_events",
		"chargers", "stations", "admin_accounts", "user_accounts",
	} {
		if _, err := db.ExecContext(ctx, "DROP TABLE IF EXISTS "+table+" CASCADE"); err != nil {
			t.Fatalf("drop %s: %v", table, err)
		}
	}

	// A lock waiter pins one pool connection while blocked on the advisory
	// lock, so the racing pool needs spare capacity beyond the waiter count.
	raceDB, err := Open(ctx, os.Getenv("NCS_TEST_PG_DSN"), 12)
	if err != nil {
		t.Fatalf("Open race pool: %v", err)
	}
	defer raceDB.Close()

	runnerDB, err := NewSQLDB(raceDB)
	if err != nil {
		t.Fatalf("NewSQLDB() error = %v", err)
	}

	const runners = 4
	reports := make([]Report, runners)
	errs := make([]error, runners)
	var group sync.WaitGroup
	start := make(chan struct{})
	for index := 0; index < runners; index++ {
		group.Add(1)
		go func(index int) {
			defer group.Done()
			<-start
			reports[index], errs[index] = Run(ctx, runnerDB, migrations.FS)
		}(index)
	}
	close(start)
	group.Wait()

	applied := 0
	for index, err := range errs {
		if err != nil {
			t.Fatalf("concurrent run %d failed: %v", index, err)
		}
		applied += len(reports[index].Applied)
	}
	expected := countMigrationFiles(t)
	if applied != expected {
		t.Fatalf("total applied across %d runners = %d, want exactly %d", runners, applied, expected)
	}

	var recorded int64
	if err := db.QueryRowContext(ctx, "SELECT count(*) FROM schema_migrations").Scan(&recorded); err != nil {
		t.Fatalf("count schema_migrations: %v", err)
	}
	if recorded != int64(expected) {
		t.Fatalf("schema_migrations rows = %d, want %d", recorded, expected)
	}
}

// countMigrationFiles counts the embedded .sql migrations so the assertion
// tracks the growing migration set.
func countMigrationFiles(t *testing.T) int {
	t.Helper()
	entries, err := fs.ReadDir(migrations.FS, ".")
	if err != nil {
		t.Fatalf("read embedded migrations: %v", err)
	}
	total := 0
	for _, entry := range entries {
		if !entry.IsDir() && strings.HasSuffix(entry.Name(), ".sql") {
			total++
		}
	}
	return total
}

func TestRunRejectsChangedMigrationOnRealDatabase(t *testing.T) {
	db, ctx := integrationDB(t)

	runnerDB, err := NewSQLDB(db)
	if err != nil {
		t.Fatalf("NewSQLDB() error = %v", err)
	}
	tampered := testingfs.MapFS{
		"0001_init.sql": &testingfs.MapFile{Data: []byte("-- tampered history\nSELECT 1;\n")},
	}
	if _, err := Run(ctx, runnerDB, tampered); err == nil {
		t.Fatal("tampered migration accepted; checksum guard is not working")
	}
}

func TestRunWorksWithSingleConnectionPool(t *testing.T) {
	db, ctx := integrationDB(t)

	// Reset to an empty database so this run applies every migration.
	for _, table := range []string{
		"schema_migrations", "order_events", "charging_orders", "wallet_transactions",
		"wallet_accounts", "operation_logs", "idempotency_records", "outbox_events",
		"chargers", "stations", "admin_accounts", "user_accounts",
	} {
		if _, err := db.ExecContext(ctx, "DROP TABLE IF EXISTS "+table+" CASCADE"); err != nil {
			t.Fatalf("drop %s: %v", table, err)
		}
	}

	// A one-connection pool is a legal production configuration; the
	// migrator must not deadlock waiting for a second connection.
	single, err := Open(ctx, os.Getenv("NCS_TEST_PG_DSN"), 1)
	if err != nil {
		t.Fatalf("Open(1) error = %v", err)
	}
	defer single.Close()

	runnerDB, err := NewSQLDB(single)
	if err != nil {
		t.Fatalf("NewSQLDB() error = %v", err)
	}
	applied, err := Run(ctx, runnerDB, migrations.FS)
	if err != nil {
		t.Fatalf("Run with a single pooled connection: %v", err)
	}
	if len(applied.Applied) != countMigrationFiles(t) {
		t.Fatalf("applied = %d, want %d", len(applied.Applied), countMigrationFiles(t))
	}

	// A second run on the same pool confirms the lock is released and the
	// database is recognized as up to date.
	repeat, err := Run(ctx, runnerDB, migrations.FS)
	if err != nil {
		t.Fatalf("second Run: %v", err)
	}
	if len(repeat.Applied) != 0 {
		t.Fatalf("second run applied %+v, want nothing", repeat.Applied)
	}
}

func TestNearbyStationOrderingAndVisibility(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewStationStore(db)
	if err != nil {
		t.Fatalf("NewStationStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)

	// Three stations at increasing distance from the query point, one
	// disabled and therefore invisible to the C-end.
	insert := func(code string, lat, lng float64, status string, idle int, price int64) int64 {
		var id int64
		if err := db.QueryRowContext(ctx,
			`INSERT INTO stations (code, name, address, latitude, longitude, status) VALUES ($1, $2, 'addr', $3, $4, $5) RETURNING id`,
			code, "站-"+code, lat, lng, status).Scan(&id); err != nil {
			t.Fatalf("seed station: %v", err)
		}
		for index := 0; index < idle+1; index++ {
			statusValue := "IDLE"
			if index >= idle {
				statusValue = "OCCUPIED"
			}
			if _, err := db.ExecContext(ctx,
				`INSERT INTO chargers (station_id, code, connector_type, power_watt, status, price_per_kwh_cents) VALUES ($1, $2, 'DC', 120000, $3, $4)`,
				id, code+"-C"+strconv.Itoa(index), statusValue, price); err != nil {
				t.Fatalf("seed charger: %v", err)
			}
		}
		return id
	}
	near := insert("NB-"+suffix, 30.0000, 104.0000, "OPEN", 2, 100)
	mid := insert("NM-"+suffix, 30.0100, 104.0100, "OPEN", 1, 200)
	insert("ND-"+suffix, 30.0001, 104.0001, "DISABLED", 5, 50)
	_ = mid

	// Distance ordering: the nearest station comes first, disabled stations
	// never appear, and each row carries distance and idle counts. The
	// keyword keeps the assertions scoped to this test's stations.
	page, err := store.ListStations(ctx, station.StationFilter{
		Page: 1, PageSize: 100,
		Latitude: 30.0, Longitude: 104.0, HasLocation: true,
		Keyword: suffix,
	})
	if err != nil {
		t.Fatalf("ListStations() error = %v", err)
	}
	if len(page.Items) != 2 {
		t.Fatalf("items = %d, want 2 (disabled station hidden)", len(page.Items))
	}
	if page.Items[0].ID != near {
		t.Fatalf("nearest station = %d, want %d", page.Items[0].ID, near)
	}
	if *page.Items[0].DistanceMeter > *page.Items[1].DistanceMeter {
		t.Fatalf("distances not ascending: %d > %d", *page.Items[0].DistanceMeter, *page.Items[1].DistanceMeter)
	}
	if page.Items[0].IdleChargerCount != 2 || page.Items[0].MinPriceCentPerKwh != 100 {
		t.Fatalf("aggregates = %#v", page.Items[0])
	}

	// Radius and idle filters narrow the result.
	page, err = store.ListStations(ctx, station.StationFilter{
		Page: 1, PageSize: 100,
		Latitude: 30.0, Longitude: 104.0, HasLocation: true,
		Keyword:      suffix,
		RadiusMeters: 500, MinIdleChargers: 2,
	})
	if err != nil || len(page.Items) != 1 || page.Items[0].ID != near {
		t.Fatalf("filtered page = %#v, %v", page, err)
	}

	// Beyond the last page the total still reports the real count (A-03 #5).
	page, err = store.ListStations(ctx, station.StationFilter{
		Page: 9, PageSize: 1,
		Latitude: 30.0, Longitude: 104.0, HasLocation: true,
		Keyword: suffix,
	})
	if err != nil {
		t.Fatalf("empty page error = %v", err)
	}
	if len(page.Items) != 0 || page.Meta.Total != 2 {
		t.Fatalf("empty page = %d items, total %d; want 0 items and total 2", len(page.Items), page.Meta.Total)
	}

	// Without coordinates the list falls back to id order with no distance.
	page, err = store.ListStations(ctx, station.StationFilter{Page: 1, PageSize: 100, Keyword: suffix})
	if err != nil || len(page.Items) != 2 || page.Items[0].DistanceMeter != nil {
		t.Fatalf("unlocated page = %#v, %v", page, err)
	}
}

func TestSMSPasswordlessRegistration(t *testing.T) {
	db, ctx := integrationDB(t)
	accountStore, err := NewAccountStore(db)
	if err != nil {
		t.Fatalf("NewAccountStore() error = %v", err)
	}
	phone := "133" + strings.ReplaceAll(uniqueSuffix(t), ".", "")[:8]

	service, err := auth.NewService(
		accountStore, accountStore,
		mustAccountMutation(t, db),
		auth.NewInMemorySessionStore(time.Minute, nil),
		auth.NewFixedWindowLimiter(100, time.Minute, nil),
		auth.NewInMemorySMSCodeStore(nil),
		time.Minute, 168*time.Hour,
	)
	if err != nil {
		t.Fatalf("auth service: %v", err)
	}
	service.SMSMock = true

	code, err := service.IssueSMSCode(ctx, phone)
	if err != nil {
		t.Fatalf("IssueSMSCode() error = %v", err)
	}
	result, err := service.LoginBySms(ctx, phone, code)
	if err != nil {
		t.Fatalf("LoginBySms() error = %v", err)
	}
	if result.Identity.Role != auth.RoleUser {
		t.Fatalf("identity = %#v", result.Identity)
	}

	// UC-U-01: the wallet row exists with a zero balance.
	var balance int64
	if err := db.QueryRowContext(ctx,
		`SELECT w.balance_cents FROM wallet_accounts w JOIN user_accounts u ON u.id = w.user_id WHERE u.phone = $1`,
		phone).Scan(&balance); err != nil {
		t.Fatalf("wallet lookup: %v", err)
	}
	if balance != 0 {
		t.Fatalf("wallet balance = %d, want 0", balance)
	}
}
