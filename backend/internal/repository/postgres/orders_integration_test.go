package postgres

import (
	"context"
	"database/sql"
	"errors"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/heguangV/charging-station-platform/backend/internal/admin"
	"github.com/heguangV/charging-station-platform/backend/internal/auth"
	"github.com/heguangV/charging-station-platform/backend/internal/order"
)

// orderFlowFixture seeds two users (the first with a funded wallet), two
// stations and one charger in station A.
func orderFlowFixture(t *testing.T, db *sql.DB, ctx context.Context, suffix string) (userA, userB, stationA, stationB, chargerA int64) {
	t.Helper()
	hash, err := auth.HashPasswordWithIterations("Dev-Password-01", 1000)
	if err != nil {
		t.Fatalf("hash: %v", err)
	}
	seedUser := func(phone string, balance int64) int64 {
		var id int64
		if err := db.QueryRowContext(ctx,
			`INSERT INTO user_accounts (phone, password_hash) VALUES ($1, $2) RETURNING id`,
			phone, hash).Scan(&id); err != nil {
			t.Fatalf("seed user: %v", err)
		}
		if _, err := db.ExecContext(ctx,
			`INSERT INTO wallet_accounts (user_id, balance_cents) VALUES ($1, $2)`, id, balance); err != nil {
			t.Fatalf("seed wallet: %v", err)
		}
		return id
	}
	userA = seedUser("135"+suffix[:9], 10000)
	userB = seedUser("134"+suffix[:9], 10000)

	if err := db.QueryRowContext(ctx,
		`INSERT INTO stations (code, name, status) VALUES ($1, '站A', 'OPEN') RETURNING id`,
		"OF-A-"+suffix).Scan(&stationA); err != nil {
		t.Fatalf("seed station A: %v", err)
	}
	if err := db.QueryRowContext(ctx,
		`INSERT INTO stations (code, name, status) VALUES ($1, '站B', 'OPEN') RETURNING id`,
		"OF-B-"+suffix).Scan(&stationB); err != nil {
		t.Fatalf("seed station B: %v", err)
	}
	if err := db.QueryRowContext(ctx,
		`INSERT INTO chargers (station_id, code, connector_type, power_watt, status, price_per_kwh_cents) VALUES ($1, 'C01', 'DC', 120000, 'IDLE', 120) RETURNING id`,
		stationA).Scan(&chargerA); err != nil {
		t.Fatalf("seed charger: %v", err)
	}
	return userA, userB, stationA, stationB, chargerA
}

func hashRequest(method, path, body string) string {
	return method + path + body // any stable string; the store only compares
}

func TestOrderFullLifecycleOnRealDatabase(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewOrderStore(db)
	if err != nil {
		t.Fatalf("NewOrderStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	userA, userB, stationA, _, chargerA := orderFlowFixture(t, db, ctx, suffix)

	// CREATE
	created, err := store.CreateOrder(ctx, order.CreateOrderCommand{
		UserID: userA, ChargerID: chargerA,
		IdempotencyKey: "create-key-0001-" + suffix, RequestHash: hashRequest("POST", "/api/v1/orders", ""), TraceID: "trace-create",
	})
	if err != nil {
		t.Fatalf("CreateOrder() error = %v", err)
	}
	if created.Status != order.StatusCreated || created.StationID != stationA || created.ChargerID != chargerA {
		t.Fatalf("created order = %#v", created)
	}
	assertChargerStatus(t, db, ctx, chargerA, "OCCUPIED")
	assertOutboxCount(t, db, ctx, created.OrderNo, order.EventOrderCreated, 1)

	// START
	started, err := store.StartCharging(ctx, order.TransitionCommand{
		UserID: userA, OrderNo: created.OrderNo,
		IdempotencyKey: "start-key-0001-" + suffix, RequestHash: hashRequest("POST", "start", ""), TraceID: "trace-start",
	})
	if err != nil {
		t.Fatalf("StartCharging() error = %v", err)
	}
	if started.Status != order.StatusStarting {
		t.Fatalf("started status = %s", started.Status)
	}
	assertOutboxCount(t, db, ctx, created.OrderNo, order.EventChargeStartRequested, 1)

	// device confirms start
	charging, err := store.ConfirmStart(ctx, order.ConfirmStartCommand{OrderNo: created.OrderNo, ChargerID: chargerA, OccurredAt: time.Now().UTC(), TraceID: "trace-device"})
	if err != nil {
		t.Fatalf("ConfirmStart() error = %v", err)
	}
	if charging.Status != order.StatusCharging {
		t.Fatalf("charging status = %s", charging.Status)
	}
	var startedAt sql.NullTime
	if err := db.QueryRowContext(ctx, `SELECT started_at FROM charging_orders WHERE order_no = $1`, created.OrderNo).Scan(&startedAt); err != nil {
		t.Fatalf("read started_at: %v", err)
	}
	if !startedAt.Valid {
		t.Fatal("started_at is NULL after ConfirmStart")
	}
	assertOutboxCount(t, db, ctx, created.OrderNo, order.EventChargeStarted, 1)

	// STOP + device stop with metered energy
	stopping, err := store.StopCharging(ctx, order.TransitionCommand{
		UserID: userA, OrderNo: created.OrderNo,
		IdempotencyKey: "stop-key-0001-" + suffix, RequestHash: hashRequest("POST", "stop", ""), TraceID: "trace-stop",
	})
	if err != nil {
		t.Fatalf("StopCharging() error = %v", err)
	}
	if stopping.Status != order.StatusStopping {
		t.Fatalf("stopping status = %s", stopping.Status)
	}

	completed, err := store.ConfirmStop(ctx, order.ConfirmStopCommand{OrderNo: created.OrderNo, ChargerID: chargerA, EnergyWh: 1500, OccurredAt: time.Now().UTC(), TraceID: "trace-device"})
	if err != nil {
		t.Fatalf("ConfirmStop() error = %v", err)
	}
	// 1.5 kWh at 120 cents/kWh = 180 cents
	if completed.Status != order.StatusCompleted || completed.EnergyWh != 1500 || completed.AmountCent != 180 {
		t.Fatalf("completed order = %#v", completed)
	}
	assertChargerStatus(t, db, ctx, chargerA, "IDLE")
	assertOutboxCount(t, db, ctx, created.OrderNo, order.EventChargeStopped, 1)
	assertOutboxCount(t, db, ctx, created.OrderNo, order.EventOrderCompleted, 1)

	// history is queryable
	page, err := store.ListOrdersByUser(ctx, order.ListFilter{UserID: userA, Page: 1, PageSize: 20, Status: order.StatusCompleted})
	if err != nil || len(page.Items) != 1 || page.Meta.Total != 1 {
		t.Fatalf("list = %#v, %v", page, err)
	}
	got, err := store.GetOrderByNo(ctx, userA, created.OrderNo)
	if err != nil || got.AmountCent != 180 {
		t.Fatalf("get = %#v, %v", got, err)
	}
	if _, err := store.GetOrderByNo(ctx, userB, created.OrderNo); !errors.Is(err, order.ErrOrderNotFound) {
		t.Fatalf("other user's order error = %v, want ErrOrderNotFound", err)
	}
}

func assertChargerStatus(t *testing.T, db *sql.DB, ctx context.Context, chargerID int64, want string) {
	t.Helper()
	var status string
	if err := db.QueryRowContext(ctx, `SELECT status FROM chargers WHERE id = $1`, chargerID).Scan(&status); err != nil {
		t.Fatalf("charger status: %v", err)
	}
	if status != want {
		t.Fatalf("charger status = %s, want %s", status, want)
	}
}

// pendingChargerCommands reads the queued device commands that name one order, oldest first.
func pendingChargerCommands(t *testing.T, db *sql.DB, ctx context.Context, orderNo string) []map[string]string {
	t.Helper()
	rows, err := db.QueryContext(ctx,
		`SELECT payload->>'command_id', payload->>'charger_id', payload->>'order_no', payload->>'action'
		   FROM outbox_events
		  WHERE event_type = $1 AND payload->>'order_no' = $2
		  ORDER BY id`,
		order.EventChargerCommandRequested, orderNo)
	if err != nil {
		t.Fatalf("read charger command events: %v", err)
	}
	defer func() { _ = rows.Close() }()

	var out []map[string]string
	for rows.Next() {
		var commandID, chargerID, readOrderNo, action sql.NullString
		if err := rows.Scan(&commandID, &chargerID, &readOrderNo, &action); err != nil {
			t.Fatalf("scan charger command event: %v", err)
		}
		out = append(out, map[string]string{
			"command_id": commandID.String, "charger_id": chargerID.String,
			"order_no": readOrderNo.String, "action": action.String,
		})
	}
	if err := rows.Err(); err != nil {
		t.Fatalf("iterate charger command events: %v", err)
	}
	return out
}

// BE-I-02: a user-driven transition that needs a device action queues that command inside the same
// transaction as the state change, and the command names the order it belongs to.
//
// Both halves matter. Without the order number no receipt could be attributed to an order, and a
// command queued in a separate transaction could be lost while the order still moved to STARTING.
func TestStartAndStopQueueTheDeviceCommandInTheSameTransaction(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewOrderStore(db)
	if err != nil {
		t.Fatalf("NewOrderStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	userA, _, _, _, chargerA := orderFlowFixture(t, db, ctx, suffix)

	created, err := store.CreateOrder(ctx, order.CreateOrderCommand{
		UserID: userA, ChargerID: chargerA,
		IdempotencyKey: "create-key-i02-" + suffix, RequestHash: hashRequest("POST", "/api/v1/orders", ""), TraceID: "trace-create",
	})
	if err != nil {
		t.Fatalf("CreateOrder() error = %v", err)
	}
	// A create is not a device action: it queues no command at all.
	if commands := pendingChargerCommands(t, db, ctx, created.OrderNo); len(commands) != 0 {
		t.Fatalf("order creation queued %d device command(s)", len(commands))
	}

	if _, err := store.StartCharging(ctx, order.TransitionCommand{
		UserID: userA, OrderNo: created.OrderNo,
		IdempotencyKey: "start-key-i02-" + suffix, RequestHash: hashRequest("POST", "start", ""), TraceID: "trace-start",
	}); err != nil {
		t.Fatalf("StartCharging() error = %v", err)
	}

	commands := pendingChargerCommands(t, db, ctx, created.OrderNo)
	if len(commands) != 1 {
		t.Fatalf("STARTING queued %d device command(s), want 1: %v", len(commands), commands)
	}
	if commands[0]["action"] != order.CommandStartCharging {
		t.Fatalf("action = %q, want %s", commands[0]["action"], order.CommandStartCharging)
	}
	if commands[0]["charger_id"] != strconv.FormatInt(chargerA, 10) {
		t.Fatalf("charger_id = %q, want %d", commands[0]["charger_id"], chargerA)
	}
	if commands[0]["command_id"] == "" {
		t.Fatal("a queued device command must carry its command id")
	}
	// The command event is unpublished work, exactly like the lifecycle event beside it.
	assertOutboxCount(t, db, ctx, strconv.FormatInt(chargerA, 10), order.EventChargerCommandRequested, 1)

	// The device confirms, then the stop queues the matching command under a new command id.
	if _, err := store.ConfirmStart(ctx, order.ConfirmStartCommand{OrderNo: created.OrderNo, ChargerID: chargerA, OccurredAt: time.Now().UTC(), TraceID: "trace-device"}); err != nil {
		t.Fatalf("ConfirmStart() error = %v", err)
	}
	if _, err := store.StopCharging(ctx, order.TransitionCommand{
		UserID: userA, OrderNo: created.OrderNo,
		IdempotencyKey: "stop-key-i02-" + suffix, RequestHash: hashRequest("POST", "stop", ""), TraceID: "trace-stop",
	}); err != nil {
		t.Fatalf("StopCharging() error = %v", err)
	}

	commands = pendingChargerCommands(t, db, ctx, created.OrderNo)
	if len(commands) != 2 {
		t.Fatalf("expected one command per device transition, got %v", commands)
	}
	if commands[1]["action"] != order.CommandStopCharging {
		t.Fatalf("action = %q, want %s", commands[1]["action"], order.CommandStopCharging)
	}
	// A reused command id would be answered from the gateway's stored outcome, so the stop would
	// replay the start instead of ever reaching the device.
	if commands[1]["command_id"] == commands[0]["command_id"] {
		t.Fatalf("the stop reused the start's command id %q", commands[0]["command_id"])
	}
}

func assertOutboxCount(t *testing.T, db *sql.DB, ctx context.Context, orderNo, eventType string, want int) {
	t.Helper()
	var count int64
	if err := db.QueryRowContext(ctx,
		`SELECT count(*) FROM outbox_events WHERE aggregate_id = $1 AND event_type = $2 AND published_at IS NULL`,
		orderNo, eventType).Scan(&count); err != nil {
		t.Fatalf("outbox count: %v", err)
	}
	if count != int64(want) {
		t.Fatalf("unpublished %s events = %d, want %d", eventType, count, want)
	}
}

func TestOrderIdempotencyReplayAndConflict(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewOrderStore(db)
	if err != nil {
		t.Fatalf("NewOrderStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	userA, _, stationA, _, chargerA := orderFlowFixture(t, db, ctx, suffix)

	key := "idem-create-0001-" + suffix
	command := order.CreateOrderCommand{UserID: userA, ChargerID: chargerA, IdempotencyKey: key, RequestHash: "hash-A"}
	first, err := store.CreateOrder(ctx, command)
	if err != nil {
		t.Fatalf("first create: %v", err)
	}

	// Same key and hash: replay the stored response, no new order.
	replay, err := store.CreateOrder(ctx, command)
	if err != nil {
		t.Fatalf("replayed create: %v", err)
	}
	if replay.OrderNo != first.OrderNo {
		t.Fatalf("replay order = %s, want %s", replay.OrderNo, first.OrderNo)
	}
	var total int64
	if err := db.QueryRowContext(ctx, `SELECT count(*) FROM charging_orders WHERE user_id = $1`, userA).Scan(&total); err != nil {
		t.Fatalf("count orders: %v", err)
	}
	if total != 1 {
		t.Fatalf("replay created a second order (%d)", total)
	}

	// Same key, different request: conflict.
	_, err = store.CreateOrder(ctx, order.CreateOrderCommand{
		UserID: userA, ChargerID: chargerA, IdempotencyKey: key, RequestHash: "hash-B"})
	if !errors.Is(err, order.ErrIdempotencyConflict) {
		t.Fatalf("conflicting hash error = %v, want ErrIdempotencyConflict", err)
	}

	// A different key on the same user hits the flow-uniqueness rule. A free
	// charger is required so the check reaches the flow phase: the first
	// order occupies chargerA.
	var chargerB int64
	if err := db.QueryRowContext(ctx,
		`INSERT INTO chargers (station_id, code, connector_type, power_watt, status, price_per_kwh_cents) VALUES ($1, 'C02', 'AC', 7000, 'IDLE', 100) RETURNING id`,
		stationA).Scan(&chargerB); err != nil {
		t.Fatalf("seed charger B: %v", err)
	}
	_, err = store.CreateOrder(ctx, order.CreateOrderCommand{
		UserID: userA, ChargerID: chargerB, IdempotencyKey: "idem-create-0002-" + suffix, RequestHash: "hash-A"})
	if !errors.Is(err, order.ErrActiveFlowExists) {
		t.Fatalf("second flow error = %v, want ErrActiveFlowExists", err)
	}

	// Start transition: same key replays, different key is a state error.
	startCommand := order.TransitionCommand{
		UserID: userA, OrderNo: first.OrderNo, IdempotencyKey: "idem-start-0001-" + suffix, RequestHash: "hash-A"}
	if _, err := store.StartCharging(ctx, startCommand); err != nil {
		t.Fatalf("first start: %v", err)
	}
	replayedStart, err := store.StartCharging(ctx, startCommand)
	if err != nil || replayedStart.Status != order.StatusStarting {
		t.Fatalf("start replay = %#v, %v", replayedStart, err)
	}
	_, err = store.StartCharging(ctx, order.TransitionCommand{
		UserID: userA, OrderNo: first.OrderNo, IdempotencyKey: "idem-start-0002-" + suffix, RequestHash: "hash-A"})
	if !errors.Is(err, order.ErrInvalidStateTransition) {
		t.Fatalf("duplicate start error = %v, want ErrInvalidStateTransition", err)
	}
}

func TestCreateOrderBusinessRulesOnRealDatabase(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewOrderStore(db)
	if err != nil {
		t.Fatalf("NewOrderStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	userA, _, stationA, stationB, chargerA := orderFlowFixture(t, db, ctx, suffix)

	// BR-04: balance below the minimum start amount is rejected.
	if _, err := db.ExecContext(ctx, `UPDATE wallet_accounts SET balance_cents = 400 WHERE user_id = $1`, userA); err != nil {
		t.Fatalf("set balance: %v", err)
	}
	_, err = store.CreateOrder(ctx, order.CreateOrderCommand{
		UserID: userA, ChargerID: chargerA, IdempotencyKey: "rule-key-0001-" + suffix, RequestHash: "h"})
	if !errors.Is(err, order.ErrInsufficientBalance) {
		t.Fatalf("low balance error = %v, want ErrInsufficientBalance", err)
	}

	// A closed station makes its chargers unavailable.
	if _, err := db.ExecContext(ctx, `UPDATE wallet_accounts SET balance_cents = 10000 WHERE user_id = $1`, userA); err != nil {
		t.Fatalf("restore balance: %v", err)
	}
	if _, err := db.ExecContext(ctx, `UPDATE stations SET status = 'CLOSED' WHERE id = $1`, stationA); err != nil {
		t.Fatalf("close station: %v", err)
	}
	_, err = store.CreateOrder(ctx, order.CreateOrderCommand{
		UserID: userA, ChargerID: chargerA, IdempotencyKey: "rule-key-0002-" + suffix, RequestHash: "h"})
	if !errors.Is(err, order.ErrChargerUnavailable) {
		t.Fatalf("closed station error = %v, want ErrChargerUnavailable", err)
	}
	if _, err := db.ExecContext(ctx, `UPDATE stations SET status = 'OPEN' WHERE id = $1`, stationA); err != nil {
		t.Fatalf("reopen station: %v", err)
	}

	// Unknown chargers are unavailable, not a 500.
	_, err = store.CreateOrder(ctx, order.CreateOrderCommand{
		UserID: userA, ChargerID: 987654, IdempotencyKey: "rule-key-0003-" + suffix, RequestHash: "h"})
	if !errors.Is(err, order.ErrChargerUnavailable) {
		t.Fatalf("unknown charger error = %v, want ErrChargerUnavailable", err)
	}

	// The composite FK still rejects station/charger mismatches inside flows.
	if _, err := db.ExecContext(ctx, `INSERT INTO charging_orders (order_no, user_id, station_id, charger_id, status)
VALUES ($1, $2, $3, $4, 'CREATED')`, "ORD-XS-"+suffix, userA, stationB, chargerA); err == nil {
		t.Fatal("cross-station order accepted by direct insert")
	} else if !strings.Contains(err.Error(), "foreign key constraint") {
		t.Fatalf("cross-station error = %v", err)
	}
	_ = stationB
}

func TestConcurrentCreateSameChargerSingleWinner(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewOrderStore(db)
	if err != nil {
		t.Fatalf("NewOrderStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	userA, userB, _, _, chargerA := orderFlowFixture(t, db, ctx, suffix)

	errs := make(chan error, 2)
	start := make(chan struct{})
	for index, user := range []int64{userA, userB} {
		go func(index int, user int64) {
			<-start
			_, err := store.CreateOrder(ctx, order.CreateOrderCommand{
				UserID:         user,
				ChargerID:      chargerA,
				IdempotencyKey: "race-" + suffix + "-" + strconv.Itoa(index),
				RequestHash:    "h" + strconv.Itoa(index),
			})
			errs <- err
		}(index, user)
	}
	close(start)

	winners := 0
	for received := 0; received < 2; received++ {
		err := <-errs
		switch {
		case err == nil:
			winners++
		case errors.Is(err, order.ErrChargerUnavailable):
			// expected loser
		default:
			t.Fatalf("unexpected race error: %v", err)
		}
	}
	if winners != 1 {
		t.Fatalf("winners = %d, want exactly 1", winners)
	}
}

func TestConcurrentCreateSameUserSingleFlow(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewOrderStore(db)
	if err != nil {
		t.Fatalf("NewOrderStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	userA, _, stationA, _, chargerA := orderFlowFixture(t, db, ctx, suffix)

	// A second idle charger so both attempts pass the charger check.
	var chargerB int64
	if err := db.QueryRowContext(ctx,
		`INSERT INTO chargers (station_id, code, connector_type, power_watt, status, price_per_kwh_cents) VALUES ($1, 'C02', 'AC', 7000, 'IDLE', 100) RETURNING id`,
		stationA).Scan(&chargerB); err != nil {
		t.Fatalf("seed charger B: %v", err)
	}

	var group sync.WaitGroup
	errs := make(chan error, 2)
	start := make(chan struct{})
	for index, charger := range []int64{chargerA, chargerB} {
		group.Add(1)
		go func(index int, charger int64) {
			defer group.Done()
			<-start
			_, err := store.CreateOrder(ctx, order.CreateOrderCommand{
				UserID:         userA,
				ChargerID:      charger,
				IdempotencyKey: "userrace-" + suffix + "-" + strconv.Itoa(index),
				RequestHash:    "h" + strconv.Itoa(index),
			})
			errs <- err
		}(index, charger)
	}
	close(start)
	group.Wait()

	winners := 0
	for received := 0; received < 2; received++ {
		err := <-errs
		switch {
		case err == nil:
			winners++
		case errors.Is(err, order.ErrActiveFlowExists):
			// expected loser
		default:
			t.Fatalf("unexpected race error: %v", err)
		}
	}
	if winners != 1 {
		t.Fatalf("flows = %d, want exactly 1", winners)
	}
}

func TestOrderStoreUsesInjectedClock(t *testing.T) {
	// Guards the store against accidentally reading the wall clock in tests.
	store := &OrderStore{clock: func() time.Time { return time.Date(2026, 1, 1, 0, 0, 0, 0, time.UTC) }}
	if store == nil {
		t.Fatal("unreachable")
	}
}

func TestOrderSettlementUsesPriceSnapshotAndWallet(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewOrderStore(db)
	if err != nil {
		t.Fatalf("NewOrderStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	userA, _, _, _, chargerA := orderFlowFixture(t, db, ctx, suffix)

	created, err := store.CreateOrder(ctx, order.CreateOrderCommand{
		UserID: userA, ChargerID: chargerA,
		IdempotencyKey: "snap-create-" + suffix, RequestHash: "h", TraceID: "trace",
	})
	if err != nil {
		t.Fatalf("CreateOrder() error = %v", err)
	}

	// The frozen requirement snapshots the price at START, not at creation.
	var snapshot int64
	if err := db.QueryRowContext(ctx,
		`SELECT price_per_kwh_cents FROM charging_orders WHERE order_no = $1`, created.OrderNo).Scan(&snapshot); err != nil {
		t.Fatalf("read snapshot before start: %v", err)
	}
	if snapshot != 0 {
		t.Fatalf("price snapshot before start = %d, want 0", snapshot)
	}
	if _, err := store.StartCharging(ctx, order.TransitionCommand{
		UserID: userA, OrderNo: created.OrderNo, IdempotencyKey: "snap-start-" + suffix, RequestHash: "h", TraceID: "t",
	}); err != nil {
		t.Fatalf("StartCharging() error = %v", err)
	}

	// The charger price changes after the start; the bill must not follow it.
	if _, err := db.ExecContext(ctx, `UPDATE chargers SET price_per_kwh_cents = 999 WHERE id = $1`, chargerA); err != nil {
		t.Fatalf("change price: %v", err)
	}
	if err := db.QueryRowContext(ctx,
		`SELECT price_per_kwh_cents FROM charging_orders WHERE order_no = $1`, created.OrderNo).Scan(&snapshot); err != nil {
		t.Fatalf("read snapshot after start: %v", err)
	}
	if snapshot != 120 {
		t.Fatalf("price snapshot = %d, want 120 (start-time tariff)", snapshot)
	}
	if _, err := store.ConfirmStart(ctx, order.ConfirmStartCommand{OrderNo: created.OrderNo, ChargerID: chargerA, OccurredAt: time.Now().UTC()}); err != nil {
		t.Fatalf("ConfirmStart() error = %v", err)
	}
	if _, err := store.StopCharging(ctx, order.TransitionCommand{
		UserID: userA, OrderNo: created.OrderNo, IdempotencyKey: "snap-stop-" + suffix, RequestHash: "h", TraceID: "t",
	}); err != nil {
		t.Fatalf("StopCharging() error = %v", err)
	}
	completed, err := store.ConfirmStop(ctx, order.ConfirmStopCommand{OrderNo: created.OrderNo, ChargerID: chargerA, EnergyWh: 1000, OccurredAt: time.Now().UTC(), TraceID: "t"})
	if err != nil {
		t.Fatalf("ConfirmStop() error = %v", err)
	}
	// 1 kWh at the snapshot 120 cents/kWh = 120 cents, not 999.
	if completed.AmountCent != 120 {
		t.Fatalf("amount = %d, want 120 from the snapshot price", completed.AmountCent)
	}

	// UC-U-09: the bill is frozen as pending payment, the wallet untouched.
	if completed.PaymentStatus != "PENDING" || completed.PaidCent != 0 {
		t.Fatalf("payment = %s/%d, want PENDING/0", completed.PaymentStatus, completed.PaidCent)
	}
	var balance int64
	if err := db.QueryRowContext(ctx, `SELECT balance_cents FROM wallet_accounts WHERE user_id = $1`, userA).Scan(&balance); err != nil {
		t.Fatalf("wallet balance: %v", err)
	}
	if balance != 10000 {
		t.Fatalf("wallet balance = %d, want untouched 10000", balance)
	}
	var billDetail int64
	if err := db.QueryRowContext(ctx,
		`SELECT count(*) FROM order_events o
JOIN charging_orders co ON co.id = o.order_id
WHERE co.order_no = $1 AND o.event_type = 'BILL_DETAIL'`,
		created.OrderNo).Scan(&billDetail); err != nil {
		t.Fatalf("bill detail: %v", err)
	}
	if billDetail != 1 {
		t.Fatalf("bill detail rows = %d, want 1", billDetail)
	}
}

func TestUnsettledOrderBlocksNewFlows(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewOrderStore(db)
	if err != nil {
		t.Fatalf("NewOrderStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	userA, _, _, _, chargerA := orderFlowFixture(t, db, ctx, suffix)

	// UC-U-09: completing a charge freezes the bill as PENDING payment and
	// releases the charger without touching the wallet.
	created, err := store.CreateOrder(ctx, order.CreateOrderCommand{
		UserID: userA, ChargerID: chargerA, IdempotencyKey: "debt-create-" + suffix, RequestHash: "h", TraceID: "t",
	})
	if err != nil {
		t.Fatalf("CreateOrder() error = %v", err)
	}
	assertChargerStatus(t, db, ctx, chargerA, "OCCUPIED")
	if _, err := store.CancelOrder(ctx, order.TransitionCommand{
		UserID: userA, OrderNo: created.OrderNo, IdempotencyKey: "debt-cancel-" + suffix, RequestHash: "h", TraceID: "t",
	}); err != nil {
		t.Fatalf("CancelOrder() error = %v", err)
	}
	assertChargerStatus(t, db, ctx, chargerA, "IDLE")

	// A cancelled order is not debt: a new flow goes through.
	second, err := store.CreateOrder(ctx, order.CreateOrderCommand{
		UserID: userA, ChargerID: chargerA, IdempotencyKey: "debt-second-" + suffix, RequestHash: "h", TraceID: "t",
	})
	if err != nil {
		t.Fatalf("create after cancel: %v", err)
	}

	// A completed order with an unpaid bill blocks every new flow for the
	// user (at most one unsettled order). Completing also releases the
	// charger, mirroring the real settlement path.
	if _, err := db.ExecContext(ctx, `UPDATE charging_orders
SET status = 'COMPLETED', amount_cents = 500, paid_cents = 0, payment_status = 'PENDING'
WHERE order_no = $1`, second.OrderNo); err != nil {
		t.Fatalf("mark pending: %v", err)
	}
	if _, err := db.ExecContext(ctx, `UPDATE chargers SET status = 'IDLE' WHERE id = $1`, chargerA); err != nil {
		t.Fatalf("release charger: %v", err)
	}
	_, err = store.CreateOrder(ctx, order.CreateOrderCommand{
		UserID: userA, ChargerID: chargerA, IdempotencyKey: "debt-third-" + suffix, RequestHash: "h", TraceID: "t",
	})
	if !errors.Is(err, order.ErrDebtOutstanding) {
		t.Fatalf("debt error = %v, want ErrDebtOutstanding", err)
	}

	// Settling the bill (PAID) re-opens the flow.
	if _, err := db.ExecContext(ctx, `UPDATE charging_orders
SET payment_status = 'PAID', paid_cents = amount_cents WHERE order_no = $1`, second.OrderNo); err != nil {
		t.Fatalf("settle: %v", err)
	}
	if _, err := store.CreateOrder(ctx, order.CreateOrderCommand{
		UserID: userA, ChargerID: chargerA, IdempotencyKey: "debt-fourth-" + suffix, RequestHash: "h", TraceID: "t",
	}); err != nil {
		t.Fatalf("create after settle: %v", err)
	}
}

func TestStartChargingRevalidatesAccountOnRealDatabase(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewOrderStore(db)
	if err != nil {
		t.Fatalf("NewOrderStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	userA, _, _, _, chargerA := orderFlowFixture(t, db, ctx, suffix)

	created, err := store.CreateOrder(ctx, order.CreateOrderCommand{
		UserID: userA, ChargerID: chargerA, IdempotencyKey: "rv-create-" + suffix, RequestHash: "h", TraceID: "t",
	})
	if err != nil {
		t.Fatalf("CreateOrder() error = %v", err)
	}

	// Frozen account cannot start.
	if _, err := db.ExecContext(ctx, `UPDATE user_accounts SET status = 'DISABLED' WHERE id = $1`, userA); err != nil {
		t.Fatalf("freeze user: %v", err)
	}
	_, err = store.StartCharging(ctx, order.TransitionCommand{
		UserID: userA, OrderNo: created.OrderNo, IdempotencyKey: "rv-start-a-" + suffix, RequestHash: "h", TraceID: "t"})
	if !errors.Is(err, order.ErrUserFrozen) {
		t.Fatalf("frozen start error = %v, want ErrUserFrozen", err)
	}
	if _, err := db.ExecContext(ctx, `UPDATE user_accounts SET status = 'ACTIVE' WHERE id = $1`, userA); err != nil {
		t.Fatalf("unfreeze user: %v", err)
	}

	// Balance drained below the minimum cannot start either.
	if _, err := db.ExecContext(ctx, `UPDATE wallet_accounts SET balance_cents = 0 WHERE user_id = $1`, userA); err != nil {
		t.Fatalf("drain wallet: %v", err)
	}
	_, err = store.StartCharging(ctx, order.TransitionCommand{
		UserID: userA, OrderNo: created.OrderNo, IdempotencyKey: "rv-start-b-" + suffix, RequestHash: "h", TraceID: "t"})
	if !errors.Is(err, order.ErrInsufficientBalance) {
		t.Fatalf("low balance start error = %v, want ErrInsufficientBalance", err)
	}
}

func TestCancelReleasesCharger(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewOrderStore(db)
	if err != nil {
		t.Fatalf("NewOrderStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	userA, _, _, _, chargerA := orderFlowFixture(t, db, ctx, suffix)

	created, err := store.CreateOrder(ctx, order.CreateOrderCommand{
		UserID: userA, ChargerID: chargerA, IdempotencyKey: "cc-create-" + suffix, RequestHash: "h", TraceID: "t",
	})
	if err != nil {
		t.Fatalf("CreateOrder() error = %v", err)
	}
	assertChargerStatus(t, db, ctx, chargerA, "OCCUPIED")

	cancelled, err := store.CancelOrder(ctx, order.TransitionCommand{
		UserID: userA, OrderNo: created.OrderNo, IdempotencyKey: "cc-cancel-" + suffix, RequestHash: "h", TraceID: "t",
	})
	if err != nil || cancelled.Status != order.StatusCancelled {
		t.Fatalf("cancel = %#v, %v", cancelled, err)
	}
	assertChargerStatus(t, db, ctx, chargerA, "IDLE")

	// The charger is free again: a second user can take it.
	userB := userA + 1
	if _, err := db.ExecContext(ctx, `INSERT INTO wallet_accounts (user_id, balance_cents)
SELECT $1, 10000 WHERE NOT EXISTS (SELECT 1 FROM wallet_accounts WHERE user_id = $1)`, userB); err != nil {
		t.Fatalf("wallet for user B: %v", err)
	}
	if _, err := store.CreateOrder(ctx, order.CreateOrderCommand{
		UserID: userB, ChargerID: chargerA, IdempotencyKey: "cc-create-b-" + suffix, RequestHash: "h", TraceID: "t",
	}); err != nil {
		t.Fatalf("create after cancel: %v", err)
	}
}

func TestExpireStaleOrdersReleasesChargers(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewOrderStore(db)
	if err != nil {
		t.Fatalf("NewOrderStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	userA, _, _, _, chargerA := orderFlowFixture(t, db, ctx, suffix)

	created, err := store.CreateOrder(ctx, order.CreateOrderCommand{
		UserID: userA, ChargerID: chargerA, IdempotencyKey: "ex-create-" + suffix, RequestHash: "h", TraceID: "t",
	})
	if err != nil {
		t.Fatalf("CreateOrder() error = %v", err)
	}
	// Backdate the request so the janitor considers it abandoned.
	if _, err := db.ExecContext(ctx,
		`UPDATE charging_orders SET requested_at = requested_at - interval '20 minutes' WHERE order_no = $1`,
		created.OrderNo); err != nil {
		t.Fatalf("backdate order: %v", err)
	}

	released, err := store.ExpireStaleOrders(ctx, 15*time.Minute)
	if err != nil {
		t.Fatalf("ExpireStaleOrders() error = %v", err)
	}
	// Other tests may have left stale orders; this order must be among them.
	if released < 1 {
		t.Fatalf("released = %d, want at least 1", released)
	}
	var status string
	if err := db.QueryRowContext(ctx, `SELECT status FROM charging_orders WHERE order_no = $1`, created.OrderNo).Scan(&status); err != nil {
		t.Fatalf("read status: %v", err)
	}
	if status != order.StatusExpired {
		t.Fatalf("status = %s, want EXPIRED", status)
	}
	assertChargerStatus(t, db, ctx, chargerA, "IDLE")
}

func TestIdempotencyRecordExpiryAllowsFreshRequest(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewOrderStore(db)
	if err != nil {
		t.Fatalf("NewOrderStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	userA, _, _, _, chargerA := orderFlowFixture(t, db, ctx, suffix)

	key := "exp-create-" + suffix
	first, err := store.CreateOrder(ctx, order.CreateOrderCommand{
		UserID: userA, ChargerID: chargerA, IdempotencyKey: key, RequestHash: "h", TraceID: "t",
	})
	if err != nil {
		t.Fatalf("first create: %v", err)
	}
	if _, err := store.CancelOrder(ctx, order.TransitionCommand{
		UserID: userA, OrderNo: first.OrderNo, IdempotencyKey: "exp-cancel-" + suffix, RequestHash: "h", TraceID: "t",
	}); err != nil {
		t.Fatalf("cancel: %v", err)
	}

	// Expire the creation record in place: after 24h the key no longer
	// replays the old response and a fresh request goes through.
	if _, err := db.ExecContext(ctx,
		`UPDATE idempotency_records SET expires_at = now() - interval '1 hour' WHERE idempotency_key = $1`, key); err != nil {
		t.Fatalf("expire record: %v", err)
	}
	second, err := store.CreateOrder(ctx, order.CreateOrderCommand{
		UserID: userA, ChargerID: chargerA, IdempotencyKey: key, RequestHash: "h", TraceID: "t",
	})
	if err != nil {
		t.Fatalf("fresh request after expiry: %v", err)
	}
	if second.OrderNo == first.OrderNo {
		t.Fatal("expired key replayed the old order")
	}
}

func TestSettleOrderCollectsPendingBill(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewOrderStore(db)
	if err != nil {
		t.Fatalf("NewOrderStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	userA, _, _, _, chargerA := orderFlowFixture(t, db, ctx, suffix)

	created, err := store.CreateOrder(ctx, order.CreateOrderCommand{
		UserID: userA, ChargerID: chargerA, IdempotencyKey: "st-create-" + suffix, RequestHash: "h", TraceID: "t",
	})
	if err != nil {
		t.Fatalf("CreateOrder() error = %v", err)
	}
	if _, err := store.StartCharging(ctx, order.TransitionCommand{
		UserID: userA, OrderNo: created.OrderNo, IdempotencyKey: "st-start-" + suffix, RequestHash: "h", TraceID: "t"}); err != nil {
		t.Fatalf("StartCharging() error = %v", err)
	}
	if _, err := store.ConfirmStart(ctx, order.ConfirmStartCommand{OrderNo: created.OrderNo, ChargerID: chargerA, OccurredAt: time.Now().UTC()}); err != nil {
		t.Fatalf("ConfirmStart() error = %v", err)
	}
	if _, err := store.StopCharging(ctx, order.TransitionCommand{
		UserID: userA, OrderNo: created.OrderNo, IdempotencyKey: "st-stop-" + suffix, RequestHash: "h", TraceID: "t"}); err != nil {
		t.Fatalf("StopCharging() error = %v", err)
	}
	if _, err := store.ConfirmStop(ctx, order.ConfirmStopCommand{OrderNo: created.OrderNo, ChargerID: chargerA, EnergyWh: 1000, OccurredAt: time.Now().UTC(), TraceID: "t"}); err != nil {
		t.Fatalf("ConfirmStop() error = %v", err)
	}

	// Confirm (UC-U-09): the pending 120 cents are collected from the wallet.
	settled, err := store.SettleOrder(ctx, order.SettleCommand{
		TransitionCommand: order.TransitionCommand{
			UserID: userA, OrderNo: created.OrderNo, IdempotencyKey: "st-confirm-" + suffix, RequestHash: "h", TraceID: "t"}})
	if err != nil {
		t.Fatalf("SettleOrder() error = %v", err)
	}
	if settled.PaymentStatus != "PAID" || settled.PaidCent != 120 {
		t.Fatalf("settled = %s/%d, want PAID/120", settled.PaymentStatus, settled.PaidCent)
	}
	var balance int64
	if err := db.QueryRowContext(ctx, `SELECT balance_cents FROM wallet_accounts WHERE user_id = $1`, userA).Scan(&balance); err != nil {
		t.Fatalf("balance: %v", err)
	}
	if balance != 10000-120 {
		t.Fatalf("balance = %d, want %d", balance, 10000-120)
	}
	var ledger int64
	if err := db.QueryRowContext(ctx, `SELECT count(*) FROM wallet_transactions WHERE user_id = $1 AND idempotency_key = $2`,
		userA, "order:"+created.OrderNo+":confirm").Scan(&ledger); err != nil {
		t.Fatalf("ledger: %v", err)
	}
	if ledger != 1 {
		t.Fatalf("ledger rows = %d, want 1", ledger)
	}

	// After settlement the user can open a new flow again.
	if _, err := store.CreateOrder(ctx, order.CreateOrderCommand{
		UserID: userA, ChargerID: chargerA, IdempotencyKey: "st-create2-" + suffix, RequestHash: "h", TraceID: "t",
	}); err != nil {
		t.Fatalf("create after settle: %v", err)
	}

	// A settled order cannot be confirmed again.
	_, err = store.SettleOrder(ctx, order.SettleCommand{
		TransitionCommand: order.TransitionCommand{
			UserID: userA, OrderNo: created.OrderNo, IdempotencyKey: "st-confirm2-" + suffix, RequestHash: "h", TraceID: "t"}})
	if !errors.Is(err, order.ErrInvalidStateTransition) {
		t.Fatalf("double settle error = %v, want ErrInvalidStateTransition", err)
	}
}

func TestOrderHistoryFiltersOnRealDatabase(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewOrderStore(db)
	if err != nil {
		t.Fatalf("NewOrderStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	userA, _, stationA, _, chargerA := orderFlowFixture(t, db, ctx, suffix)

	created, err := store.CreateOrder(ctx, order.CreateOrderCommand{
		UserID: userA, ChargerID: chargerA, IdempotencyKey: "hf-create-" + suffix, RequestHash: "h", TraceID: "t",
	})
	if err != nil {
		t.Fatalf("CreateOrder() error = %v", err)
	}

	byOrderNo, err := store.ListOrdersByUser(ctx, order.ListFilter{UserID: userA, Page: 1, PageSize: 100, Status: "CREATED"})
	if err != nil || len(byOrderNo.Items) != 1 || byOrderNo.Items[0].OrderNo != created.OrderNo {
		t.Fatalf("byOrderNo = %#v, %v", byOrderNo, err)
	}

	byStation, err := store.ListOrdersByUser(ctx, order.ListFilter{UserID: userA, Page: 1, PageSize: 100, StationID: stationA, StationIDSet: true})
	if err != nil || len(byStation.Items) != 1 || byStation.Meta.Total != 1 {
		t.Fatalf("station filter = %#v, %v", byStation, err)
	}
	other, err := store.ListOrdersByUser(ctx, order.ListFilter{UserID: userA, Page: 1, PageSize: 100, StationID: stationA + 999, StationIDSet: true})
	if err != nil || len(other.Items) != 0 || other.Meta.Total != 0 {
		t.Fatalf("wrong station filter = %#v, %v", other, err)
	}
	byPayment, err := store.ListOrdersByUser(ctx, order.ListFilter{UserID: userA, Page: 1, PageSize: 100, PaymentStatus: "PENDING"})
	if err != nil || len(byPayment.Items) != 1 || byPayment.Meta.Total != 1 {
		t.Fatalf("payment filter = %#v, %v", byPayment, err)
	}
	// A page beyond the last row still reports the real total.
	empty, err := store.ListOrdersByUser(ctx, order.ListFilter{UserID: userA, Page: 9, PageSize: 1, PaymentStatus: "PENDING"})
	if err != nil || len(empty.Items) != 0 || empty.Meta.Total != 1 {
		t.Fatalf("empty page = %#v, %v", empty, err)
	}
	// The list carries the payment fields.
	if byPayment.Items[0].PaymentStatus != "PENDING" {
		t.Fatalf("list payment status = %q", byPayment.Items[0].PaymentStatus)
	}
}

func TestSTARTINGCancelKeepsChargerUntilRevocationConfirmed(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewOrderStore(db)
	if err != nil {
		t.Fatalf("NewOrderStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	userA, _, _, _, chargerA := orderFlowFixture(t, db, ctx, suffix)

	created, err := store.CreateOrder(ctx, order.CreateOrderCommand{
		UserID: userA, ChargerID: chargerA, IdempotencyKey: "rc-create-" + suffix, RequestHash: "h", TraceID: "t",
	})
	if err != nil {
		t.Fatalf("CreateOrder() error = %v", err)
	}
	if _, err := store.StartCharging(ctx, order.TransitionCommand{
		UserID: userA, OrderNo: created.OrderNo, IdempotencyKey: "rc-start-" + suffix, RequestHash: "h", TraceID: "t"}); err != nil {
		t.Fatalf("StartCharging() error = %v", err)
	}

	// Cancelling a STARTING order queues the revocation but must NOT release
	// the charger: the device may still be charging physically.
	if _, err := store.CancelOrder(ctx, order.TransitionCommand{
		UserID: userA, OrderNo: created.OrderNo, IdempotencyKey: "rc-cancel-" + suffix, RequestHash: "h", TraceID: "t"}); err != nil {
		t.Fatalf("CancelOrder() error = %v", err)
	}
	assertChargerStatus(t, db, ctx, chargerA, admin.ChargerStatusOccupied)

	// The orphan sweep must skip it as well.
	if _, err := store.ReleaseOrphanedChargers(ctx); err != nil {
		t.Fatalf("ReleaseOrphanedChargers() error = %v", err)
	}
	assertChargerStatus(t, db, ctx, chargerA, admin.ChargerStatusOccupied)

	// Only a device confirmation that COMPLETED releases the charger.
	released, err := store.CompleteChargerCommand(ctx, chargerA, "RESTART", "COMPLETED")
	if err != nil || !released {
		t.Fatalf("CompleteChargerCommand = %v, %v; want released", released, err)
	}
	assertChargerStatus(t, db, ctx, chargerA, "IDLE")

	// A duplicate confirmation is a no-op.
	released, err = store.CompleteChargerCommand(ctx, chargerA, "RESTART", "COMPLETED")
	if err != nil || released {
		t.Fatalf("duplicate completion = %v, %v; want no-op", released, err)
	}
}

// TestOrderListSortAndWindowOnRealDatabase covers the two query features the
// front end's order list needs and that the contract registers: the creation-time
// window (createdFrom/createdTo) and the page order (sort). Both are asserted
// against real rows, because a filter that is accepted but not applied is
// indistinguishable from a working one at the HTTP edge.
func TestOrderListSortAndWindowOnRealDatabase(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewOrderStore(db)
	if err != nil {
		t.Fatalf("NewOrderStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	userA, _, _, _, chargerA := orderFlowFixture(t, db, ctx, suffix)

	// The first order is settled (a user may only have one active flow), which
	// also gives the second order a different amount and a later created_at.
	olderNo := b07SettledOrder(t, db, ctx, store, userA, chargerA, suffix)
	newer, err := store.CreateOrder(ctx, order.CreateOrderCommand{
		UserID: userA, ChargerID: chargerA, IdempotencyKey: "b07-list-sort-" + suffix, RequestHash: "h", TraceID: "t",
	})
	if err != nil {
		t.Fatalf("second CreateOrder() error = %v", err)
	}

	descending, err := store.ListOrdersByUser(ctx, order.ListFilter{UserID: userA, Page: 1, PageSize: 100})
	if err != nil || len(descending.Items) != 2 {
		t.Fatalf("default order = %d items, %v; want 2 (newest first)", len(descending.Items), err)
	}
	if descending.Items[0].OrderNo != newer.OrderNo {
		t.Fatalf("default order first item = %s, want the newest %s", descending.Items[0].OrderNo, newer.OrderNo)
	}

	ascending, err := store.ListOrdersByUser(ctx, order.ListFilter{UserID: userA, Page: 1, PageSize: 100, Sort: order.SortCreatedAtAsc})
	if err != nil || len(ascending.Items) != 2 {
		t.Fatalf("ascending order = %d items, %v; want 2", len(ascending.Items), err)
	}
	if ascending.Items[0].OrderNo != olderNo {
		t.Fatalf("ascending first item = %s, want the oldest %s", ascending.Items[0].OrderNo, olderNo)
	}
	if !ascending.Items[0].CreatedAt.Before(ascending.Items[1].CreatedAt) {
		t.Fatalf("ascending created_at = %s then %s", ascending.Items[0].CreatedAt, ascending.Items[1].CreatedAt)
	}

	// A window that starts in the future and one that ends in the past both match
	// nothing; a window that starts an hour ago matches both rows.
	future := time.Now().UTC().Add(time.Hour)
	if page, err := store.ListOrdersByUser(ctx, order.ListFilter{UserID: userA, Page: 1, PageSize: 100, CreatedFrom: &future}); err != nil {
		t.Fatalf("future window error = %v", err)
	} else if len(page.Items) != 0 || page.Meta.Total != 0 {
		t.Fatalf("future window items/total = %d/%d, want 0/0", len(page.Items), page.Meta.Total)
	}
	past := time.Now().UTC().Add(-time.Hour)
	if page, err := store.ListOrdersByUser(ctx, order.ListFilter{UserID: userA, Page: 1, PageSize: 100, CreatedTo: &past}); err != nil {
		t.Fatalf("past window error = %v", err)
	} else if len(page.Items) != 0 || page.Meta.Total != 0 {
		t.Fatalf("past window items/total = %d/%d, want 0/0", len(page.Items), page.Meta.Total)
	}
	if page, err := store.ListOrdersByUser(ctx, order.ListFilter{UserID: userA, Page: 1, PageSize: 100, CreatedFrom: &past}); err != nil {
		t.Fatalf("recent window error = %v", err)
	} else if len(page.Items) != 2 || page.Meta.Total != 2 {
		t.Fatalf("recent window items/total = %d/%d, want 2/2", len(page.Items), page.Meta.Total)
	}
}
