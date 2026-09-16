package postgres

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"strconv"
	"testing"
	"time"

	"github.com/heguangV/charging-station-platform/backend/internal/order"
)

// These tests drive the BE-I-02 receipt path against a real PostgreSQL: the device facts are applied
// through the same transactions the endpoint uses, so what they assert is what the database really
// holds afterwards - the state, the fact times, the bill and the audit trail.

// receiptFixture prepares one order that is waiting for the device to confirm the start.
type receiptFixture struct {
	store     *OrderStore
	orderNo   string
	chargerID int64
	userID    int64
}

func newReceiptFixture(t *testing.T, db *sql.DB, ctx context.Context) receiptFixture {
	t.Helper()
	store, err := NewOrderStore(db)
	if err != nil {
		t.Fatalf("NewOrderStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	userA, _, _, _, chargerA := orderFlowFixture(t, db, ctx, suffix)

	created, err := store.CreateOrder(ctx, order.CreateOrderCommand{
		UserID: userA, ChargerID: chargerA,
		IdempotencyKey: "receipt-create-" + suffix, RequestHash: "h", TraceID: "trace-create",
	})
	if err != nil {
		t.Fatalf("CreateOrder() error = %v", err)
	}
	if _, err := store.StartCharging(ctx, order.TransitionCommand{
		UserID: userA, OrderNo: created.OrderNo,
		IdempotencyKey: "receipt-start-" + suffix, RequestHash: "h", TraceID: "trace-start",
	}); err != nil {
		t.Fatalf("StartCharging() error = %v", err)
	}
	return receiptFixture{store: store, orderNo: created.OrderNo, chargerID: chargerA, userID: userA}
}

// chargerCommandsWithAction narrows the queued device commands to one action, which is what the
// recovery sweep's attempt count is based on.
func chargerCommandsWithAction(t *testing.T, db *sql.DB, ctx context.Context, orderNo, action string) []map[string]string {
	t.Helper()
	rows, err := db.QueryContext(ctx,
		`SELECT payload->>'command_id', payload->>'charger_id', payload->>'order_no', payload->>'action'
		   FROM outbox_events
		  WHERE event_type = $1 AND payload->>'order_no' = $2 AND payload->>'action' = $3
		  ORDER BY id`,
		order.EventChargerCommandRequested, orderNo, action)
	if err != nil {
		t.Fatalf("read charger command events: %v", err)
	}
	defer func() { _ = rows.Close() }()

	var out []map[string]string
	for rows.Next() {
		var commandID, chargerID, readOrderNo, readAction sql.NullString
		if err := rows.Scan(&commandID, &chargerID, &readOrderNo, &readAction); err != nil {
			t.Fatalf("scan charger command event: %v", err)
		}
		out = append(out, map[string]string{
			"command_id": commandID.String, "charger_id": chargerID.String,
			"order_no": readOrderNo.String, "action": readAction.String,
		})
	}
	if err := rows.Err(); err != nil {
		t.Fatalf("iterate charger command events: %v", err)
	}
	return out
}

func orderStatus(t *testing.T, db *sql.DB, ctx context.Context, orderNo string) string {
	t.Helper()
	var status string
	if err := db.QueryRowContext(ctx, `SELECT status FROM charging_orders WHERE order_no = $1`, orderNo).Scan(&status); err != nil {
		t.Fatalf("read order status: %v", err)
	}
	return status
}

func orderFactTimes(t *testing.T, db *sql.DB, ctx context.Context, orderNo string) (started, stopped sql.NullTime) {
	t.Helper()
	if err := db.QueryRowContext(ctx,
		`SELECT started_at, stopped_at FROM charging_orders WHERE order_no = $1`, orderNo).Scan(&started, &stopped); err != nil {
		t.Fatalf("read fact times: %v", err)
	}
	return started, stopped
}

func auditRows(t *testing.T, db *sql.DB, ctx context.Context, orderNo, eventType string) int {
	t.Helper()
	var count int
	if err := db.QueryRowContext(ctx, `SELECT count(*)
   FROM order_events e
   JOIN charging_orders o ON o.id = e.order_id
  WHERE o.order_no = $1 AND e.event_type = $2`, orderNo, eventType).Scan(&count); err != nil {
		t.Fatalf("count %s rows: %v", eventType, err)
	}
	return count
}

// The device's fact time is what is written, not the moment the platform received the receipt. Both
// ends of the interval matter: a delayed or offline report must be billed in the window the charge
// really happened in, so the start and the stop each carry the device's own timestamp.
func TestChargerReceiptWritesTheDeviceFactTime(t *testing.T) {
	db, ctx := integrationDB(t)
	fixture := newReceiptFixture(t, db, ctx)

	startedAt := time.Now().UTC().Add(-30 * time.Minute).Truncate(time.Millisecond)
	if _, err := fixture.store.ConfirmStart(ctx, order.ConfirmStartCommand{
		OrderNo: fixture.orderNo, ChargerID: fixture.chargerID, OccurredAt: startedAt,
		EventID: "evt_fact_start_" + fixture.orderNo, RequestHash: "digest-start-" + fixture.orderNo, TraceID: "trace-start",
	}); err != nil {
		t.Fatalf("ConfirmStart() error = %v", err)
	}
	started, _ := orderFactTimes(t, db, ctx, fixture.orderNo)
	if !started.Valid {
		t.Fatal("started_at is NULL after a start confirmation")
	}
	if !started.Time.UTC().Equal(startedAt) {
		t.Fatalf("started_at = %s, want the device fact time %s", started.Time.UTC(), startedAt)
	}

	if _, err := fixture.store.StopCharging(ctx, order.TransitionCommand{
		UserID: fixture.userID, OrderNo: fixture.orderNo,
		IdempotencyKey: "receipt-stop", RequestHash: "h", TraceID: "trace-stop",
	}); err != nil {
		t.Fatalf("StopCharging() error = %v", err)
	}

	// The stop fact time is a minute after the start and also in the past, so the receipt is a
	// delayed report rather than a live one.
	stoppedAt := startedAt.Add(time.Minute)
	completed, err := fixture.store.ConfirmStop(ctx, order.ConfirmStopCommand{
		OrderNo: fixture.orderNo, ChargerID: fixture.chargerID, EnergyWh: 1000, OccurredAt: stoppedAt,
		EventID: "evt_fact_stop_" + fixture.orderNo, RequestHash: "digest-stop-" + fixture.orderNo, TraceID: "trace-stop",
	})
	if err != nil {
		t.Fatalf("ConfirmStop() error = %v", err)
	}
	if completed.Status != order.StatusCompleted {
		t.Fatalf("status = %s, want COMPLETED", completed.Status)
	}
	_, stopped := orderFactTimes(t, db, ctx, fixture.orderNo)
	if !stopped.Valid || !stopped.Time.UTC().Equal(stoppedAt) {
		t.Fatalf("stopped_at = %v, want the device fact time %s", stopped, stoppedAt)
	}
	var completedAt sql.NullTime
	if err := db.QueryRowContext(ctx, `SELECT completed_at FROM charging_orders WHERE order_no = $1`, fixture.orderNo).Scan(&completedAt); err != nil {
		t.Fatalf("read completed_at: %v", err)
	}
	if !completedAt.Valid || !completedAt.Time.UTC().Equal(stoppedAt) {
		t.Fatalf("completed_at = %v, want the same fact time as the stop", completedAt)
	}
	assertChargerStatus(t, db, ctx, fixture.chargerID, "IDLE")
}

// The fact time decides the tariff window, so the same charge billed at a different time costs a
// different amount. This is the reason occurredAt is not an audit field.
//
// The window is configured as wall-clock hours in the deployment's billing timezone (here the
// product default, +08), which is what an operator means by 23:00-07:00. Pricing the same window
// against UTC hours - the previous behaviour - billed a charge at 12:00 local (04:00 UTC) at the
// off-peak price, and a real 23:00-07:00 local charge at the peak price.
func TestChargerReceiptFactTimeDrivesTimeOfUseBilling(t *testing.T) {
	db, ctx := integrationDB(t)
	billing := order.DefaultBillingLocation()

	// A tariff with a cheap off-peak window between 23:00 and 07:00 local. The window and the prices
	// are read from the charger when the start transaction snapshots them, so they are set before
	// the order starts charging.
	charge := func(t *testing.T, startUTC time.Time, label string) int64 {
		t.Helper()
		fixture := newReceiptFixture(t, db, ctx)
		if _, err := db.ExecContext(ctx, `UPDATE chargers
   SET price_per_kwh_cents = 120, service_price_per_kwh_cents = 0,
       off_peak_electricity_price_per_kwh_cents = 50, off_peak_start_hour = 23, off_peak_end_hour = 7
 WHERE id = $1`, fixture.chargerID); err != nil {
			t.Fatalf("configure tariff: %v", err)
		}
		// The snapshot is taken by StartCharging: re-run the transition so the configured tariff is
		// the one this order is billed with (the fixture already started it with the default one).
		if _, err := db.ExecContext(ctx, `UPDATE charging_orders SET status = 'CREATED' WHERE order_no = $1`, fixture.orderNo); err != nil {
			t.Fatalf("reset order status: %v", err)
		}
		if _, err := fixture.store.StartCharging(ctx, order.TransitionCommand{
			UserID: fixture.userID, OrderNo: fixture.orderNo,
			IdempotencyKey: "tou-start-" + label, RequestHash: "h", TraceID: "trace",
		}); err != nil {
			t.Fatalf("StartCharging() error = %v", err)
		}
		if _, err := fixture.store.ConfirmStart(ctx, order.ConfirmStartCommand{
			OrderNo: fixture.orderNo, ChargerID: fixture.chargerID, OccurredAt: startUTC,
			EventID: "evt_tou_start_" + label + "_" + fixture.orderNo, RequestHash: "d-" + fixture.orderNo, TraceID: "trace",
		}); err != nil {
			t.Fatalf("ConfirmStart() error = %v", err)
		}
		if _, err := fixture.store.StopCharging(ctx, order.TransitionCommand{
			UserID: fixture.userID, OrderNo: fixture.orderNo,
			IdempotencyKey: "tou-stop-" + label, RequestHash: "h", TraceID: "trace",
		}); err != nil {
			t.Fatalf("StopCharging() error = %v", err)
		}
		completed, err := fixture.store.ConfirmStop(ctx, order.ConfirmStopCommand{
			OrderNo: fixture.orderNo, ChargerID: fixture.chargerID, EnergyWh: 1000,
			OccurredAt: startUTC.Add(time.Hour),
			EventID:    "evt_tou_stop_" + label + "_" + fixture.orderNo, RequestHash: "d-" + fixture.orderNo, TraceID: "trace",
		})
		if err != nil {
			t.Fatalf("ConfirmStop() error = %v", err)
		}
		return completed.AmountCent
	}

	now := time.Now().In(billing)
	night := time.Date(now.Year(), now.Month(), now.Day(), 23, 30, 0, 0, billing)
	midday := time.Date(now.Year(), now.Month(), now.Day(), 12, 0, 0, 0, billing)

	offPeak := charge(t, night, "offpeak")
	peak := charge(t, midday, "peak")

	// 1 kWh at 50 cents off-peak and at 120 cents in the peak window; the service fee is zero.
	if offPeak != 50 {
		t.Fatalf("off-peak charge cost %d cents, want 50 (the local fact time must pick the cheap window)", offPeak)
	}
	// 12:00 local is 04:00 UTC: inside a 00:00-08:00 UTC window, outside a 23:00-07:00 local one.
	// Billing the window in UTC used to charge this one at the off-peak price.
	if peak != 120 {
		t.Fatalf("peak charge cost %d cents, want 120 (the window is local, not UTC)", peak)
	}
}

// A gateway that does not get an answer sends the same receipt again. Applying it twice would
// advance an already advanced order and, for a stop, bill the same charge twice.
func TestChargerReceiptIsIdempotent(t *testing.T) {
	db, ctx := integrationDB(t)
	fixture := newReceiptFixture(t, db, ctx)
	occurredAt := time.Now().UTC().Add(-time.Minute).Truncate(time.Millisecond)
	command := order.ConfirmStartCommand{
		OrderNo: fixture.orderNo, ChargerID: fixture.chargerID, OccurredAt: occurredAt,
		EventID: "evt_duplicate_start_" + fixture.orderNo, RequestHash: "digest-1-" + fixture.orderNo, TraceID: "trace",
	}

	first, err := fixture.store.ConfirmStart(ctx, command)
	if err != nil {
		t.Fatalf("first ConfirmStart() error = %v", err)
	}
	if first.Status != order.StatusCharging {
		t.Fatalf("first status = %s, want CHARGING", first.Status)
	}

	second, err := fixture.store.ConfirmStart(ctx, command)
	if err != nil {
		t.Fatalf("duplicate ConfirmStart() error = %v", err)
	}
	if second.Status != first.Status || second.OrderNo != first.OrderNo {
		t.Fatalf("a duplicate must be answered with the first result, got %+v want %+v", second, first)
	}
	// Exactly one fact was recorded, so the platform told the world about it once.
	assertOutboxCount(t, db, ctx, fixture.orderNo, order.EventChargeStarted, 1)

	// The same receipt id with a different payload is a conflict, not a replay: the stored result
	// belongs to the first fact.
	conflicting := command
	conflicting.RequestHash = "digest-2-" + fixture.orderNo
	if _, err := fixture.store.ConfirmStart(ctx, conflicting); !errors.Is(err, order.ErrIdempotencyConflict) {
		t.Fatalf("error = %v, want ErrIdempotencyConflict", err)
	}
}

// A receipt that contradicts the order is refused, audited, and leaves nothing behind - including
// no claim on its own receipt id, because nothing was applied under it.
func TestChargerReceiptRejectionsAreAuditedAndChangeNothing(t *testing.T) {
	db, ctx := integrationDB(t)
	fixture := newReceiptFixture(t, db, ctx)
	now := time.Now().UTC().Add(-time.Minute)

	// A stop before the start: the order is STARTING, so there is nothing to complete.
	if _, err := fixture.store.ConfirmStop(ctx, order.ConfirmStopCommand{
		OrderNo: fixture.orderNo, ChargerID: fixture.chargerID, EnergyWh: 1000, OccurredAt: now,
		EventID: "evt_out_of_order_" + fixture.orderNo, RequestHash: "digest-" + fixture.orderNo, TraceID: "trace",
	}); !errors.Is(err, order.ErrFactTimeOutOfOrder) {
		t.Fatalf("out-of-order stop error = %v, want ErrFactTimeOutOfOrder", err)
	}
	if got := orderStatus(t, db, ctx, fixture.orderNo); got != order.StatusStarting {
		t.Fatalf("the order moved to %s on a refused receipt", got)
	}
	if audit := auditRows(t, db, ctx, fixture.orderNo, "CHARGER_EVENT_REJECTED"); audit != 1 {
		t.Fatalf("expected 1 audit row for the refused receipt, got %d", audit)
	}

	// A receipt for a charger that does not serve this order.
	otherCharger := fixture.chargerID + 1000
	if _, err := fixture.store.ConfirmStart(ctx, order.ConfirmStartCommand{
		OrderNo: fixture.orderNo, ChargerID: otherCharger, OccurredAt: now,
		EventID: "evt_other_charger_" + fixture.orderNo, RequestHash: "digest-" + fixture.orderNo, TraceID: "trace",
	}); !errors.Is(err, order.ErrChargerOrderMismatch) {
		t.Fatalf("charger mismatch error = %v, want ErrChargerOrderMismatch", err)
	}
	if audit := auditRows(t, db, ctx, fixture.orderNo, "CHARGER_EVENT_REJECTED"); audit != 2 {
		t.Fatalf("expected 2 audit rows, got %d", audit)
	}

	// An unknown order.
	if _, err := fixture.store.ConfirmStart(ctx, order.ConfirmStartCommand{
		OrderNo: "ORD20260914120000zzzz", ChargerID: fixture.chargerID, OccurredAt: now,
		EventID: "evt_missing_order_" + fixture.orderNo, RequestHash: "digest-" + fixture.orderNo, TraceID: "trace",
	}); !errors.Is(err, order.ErrOrderNotFound) {
		t.Fatalf("unknown order error = %v, want ErrOrderNotFound", err)
	}

	// A refused receipt does not consume its id: the same id still applies once the order is
	// ready for it. Nothing was applied under that id, so remembering it as applied would be a
	// fiction.
	if _, err := fixture.store.ConfirmStart(ctx, order.ConfirmStartCommand{
		OrderNo: fixture.orderNo, ChargerID: fixture.chargerID, OccurredAt: now,
		EventID: "evt_out_of_order_" + fixture.orderNo, RequestHash: "digest-" + fixture.orderNo, TraceID: "trace",
	}); err != nil {
		t.Fatalf("a refused receipt id must stay usable, got %v", err)
	}
	if got := orderStatus(t, db, ctx, fixture.orderNo); got != order.StatusCharging {
		t.Fatalf("the order did not reach CHARGING, got %s", got)
	}
}

// A stop the device reports before the recorded start is impossible and refused: the bill would be
// computed over a negative interval.
func TestChargerReceiptRejectsAStopBeforeTheRecordedStart(t *testing.T) {
	db, ctx := integrationDB(t)
	fixture := newReceiptFixture(t, db, ctx)
	startedAt := time.Now().UTC().Add(-10 * time.Minute).Truncate(time.Millisecond)

	if _, err := fixture.store.ConfirmStart(ctx, order.ConfirmStartCommand{
		OrderNo: fixture.orderNo, ChargerID: fixture.chargerID, OccurredAt: startedAt,
		EventID: "evt_order_start_" + fixture.orderNo, RequestHash: "d-" + fixture.orderNo, TraceID: "trace",
	}); err != nil {
		t.Fatalf("ConfirmStart() error = %v", err)
	}
	if _, err := fixture.store.StopCharging(ctx, order.TransitionCommand{
		UserID: fixture.userID, OrderNo: fixture.orderNo,
		IdempotencyKey: "before-start-stop", RequestHash: "h", TraceID: "trace",
	}); err != nil {
		t.Fatalf("StopCharging() error = %v", err)
	}

	if _, err := fixture.store.ConfirmStop(ctx, order.ConfirmStopCommand{
		OrderNo: fixture.orderNo, ChargerID: fixture.chargerID, EnergyWh: 1000,
		OccurredAt: startedAt.Add(-time.Minute),
		EventID:    "evt_before_start_" + fixture.orderNo, RequestHash: "d-" + fixture.orderNo, TraceID: "trace",
	}); !errors.Is(err, order.ErrFactTimeOutOfOrder) {
		t.Fatalf("error = %v, want ErrFactTimeOutOfOrder", err)
	}
	if got := orderStatus(t, db, ctx, fixture.orderNo); got != order.StatusStopping {
		t.Fatalf("the order moved to %s on a refused stop", got)
	}
	if audit := auditRows(t, db, ctx, fixture.orderNo, "CHARGER_EVENT_REJECTED"); audit != 1 {
		t.Fatalf("expected the refused stop to be audited, got %d rows", audit)
	}
}

// The frozen failure semantics: a command the device accepted changes nothing, an explicit START
// failure fails the order, and an explicit STOP failure keeps the order STOPPING because the device
// may still be charging.
func TestChargeCommandFailureSemantics(t *testing.T) {
	db, ctx := integrationDB(t)

	t.Run("an accepted start command advances nothing", func(t *testing.T) {
		fixture := newReceiptFixture(t, db, ctx)
		applied, err := fixture.store.RecordChargerCommandResult(ctx, "cmd_accepted_"+fixture.orderNo, fixture.orderNo,
			fixture.chargerID, order.CommandStartCharging, "COMPLETED", "trace")
		if err != nil {
			t.Fatalf("RecordChargerCommandResult() error = %v", err)
		}
		if applied {
			t.Fatal("command acceptance is not a state change and must not be applied as one")
		}
		if got := orderStatus(t, db, ctx, fixture.orderNo); got != order.StatusStarting {
			t.Fatalf("status = %s, want STARTING: only the receipt may advance the order", got)
		}
		if got := chargerStatus(t, db, ctx, fixture.chargerID); got != "OCCUPIED" {
			t.Fatalf("charger status = %s, want OCCUPIED", got)
		}
	})

	t.Run("a failed start fails the order and parks the charger", func(t *testing.T) {
		fixture := newReceiptFixture(t, db, ctx)
		applied, err := fixture.store.RecordChargerCommandResult(ctx, "cmd_start_failed_"+fixture.orderNo, fixture.orderNo,
			fixture.chargerID, order.CommandStartCharging, "FAILED", "trace")
		if err != nil {
			t.Fatalf("RecordChargerCommandResult() error = %v", err)
		}
		if !applied {
			t.Fatal("an explicit start failure must be applied")
		}
		if got := orderStatus(t, db, ctx, fixture.orderNo); got != order.StatusFailed {
			t.Fatalf("status = %s, want FAILED", got)
		}
		if got := chargerStatus(t, db, ctx, fixture.chargerID); got != "FAULT" {
			t.Fatalf("charger status = %s, want FAULT", got)
		}
		// The completion event carries the outcome so the B-line consumer sees the same fact.
		var payload []byte
		if err := db.QueryRowContext(ctx, `SELECT payload FROM outbox_events
 WHERE event_type = 'CHARGER_COMMAND_COMPLETED' AND aggregate_id = $1 ORDER BY id DESC LIMIT 1`,
			strconv.FormatInt(fixture.chargerID, 10)).Scan(&payload); err != nil {
			t.Fatalf("read completion event: %v", err)
		}
		var decoded map[string]string
		if err := json.Unmarshal(payload, &decoded); err != nil {
			t.Fatalf("decode completion payload: %v (%s)", err, payload)
		}
		if decoded["result"] != "FAILED" || decoded["action"] != order.CommandStartCharging {
			t.Fatalf("unexpected completion payload %v", decoded)
		}
		if decoded["command_id"] != "cmd_start_failed_"+fixture.orderNo {
			t.Fatalf("the completion must name the command it completes, got %v", decoded)
		}
	})

	t.Run("a failed stop keeps the order stopping", func(t *testing.T) {
		fixture := newReceiptFixture(t, db, ctx)
		if _, err := fixture.store.ConfirmStart(ctx, order.ConfirmStartCommand{
			OrderNo: fixture.orderNo, ChargerID: fixture.chargerID, OccurredAt: time.Now().UTC().Add(-time.Minute),
			EventID: "evt_stop_fail_start_" + fixture.orderNo, RequestHash: "d-" + fixture.orderNo, TraceID: "trace",
		}); err != nil {
			t.Fatalf("ConfirmStart() error = %v", err)
		}
		if _, err := fixture.store.StopCharging(ctx, order.TransitionCommand{
			UserID: fixture.userID, OrderNo: fixture.orderNo,
			IdempotencyKey: "stop-fail-stop", RequestHash: "h", TraceID: "trace",
		}); err != nil {
			t.Fatalf("StopCharging() error = %v", err)
		}

		applied, err := fixture.store.RecordChargerCommandResult(ctx, "cmd_stop_failed_"+fixture.orderNo, fixture.orderNo,
			fixture.chargerID, order.CommandStopCharging, "FAILED", "trace")
		if err != nil {
			t.Fatalf("RecordChargerCommandResult() error = %v", err)
		}
		if !applied {
			t.Fatal("an explicit stop failure must be applied")
		}
		if got := orderStatus(t, db, ctx, fixture.orderNo); got != order.StatusStopping {
			t.Fatalf("status = %s, want STOPPING: the platform must not assume the device stopped", got)
		}
		if got := chargerStatus(t, db, ctx, fixture.chargerID); got != "FAULT" {
			t.Fatalf("charger status = %s, want FAULT", got)
		}
		// Nothing automatic closes the order: it is not settled and its energy stays unknown.
		var energy int64
		var payment string
		if err := db.QueryRowContext(ctx,
			`SELECT energy_wh, payment_status FROM charging_orders WHERE order_no = $1`, fixture.orderNo).
			Scan(&energy, &payment); err != nil {
			t.Fatalf("read order: %v", err)
		}
		if energy != 0 || payment == "PAID" {
			t.Fatalf("energy = %d, payment = %s: a failed stop must not close the order", energy, payment)
		}
	})

	t.Run("a stale start failure does not fail a running charge", func(t *testing.T) {
		fixture := newReceiptFixture(t, db, ctx)
		if _, err := fixture.store.ConfirmStart(ctx, order.ConfirmStartCommand{
			OrderNo: fixture.orderNo, ChargerID: fixture.chargerID, OccurredAt: time.Now().UTC().Add(-time.Minute),
			EventID: "evt_stale_start_" + fixture.orderNo, RequestHash: "d-" + fixture.orderNo, TraceID: "trace",
		}); err != nil {
			t.Fatalf("ConfirmStart() error = %v", err)
		}

		applied, err := fixture.store.RecordChargerCommandResult(ctx, "cmd_stale_"+fixture.orderNo, fixture.orderNo,
			fixture.chargerID, order.CommandStartCharging, "FAILED", "trace")
		if err != nil {
			t.Fatalf("RecordChargerCommandResult() error = %v", err)
		}
		if applied {
			t.Fatal("an outcome the order already moved past must not be applied")
		}
		if got := orderStatus(t, db, ctx, fixture.orderNo); got != order.StatusCharging {
			t.Fatalf("status = %s, want CHARGING", got)
		}
		if got := chargerStatus(t, db, ctx, fixture.chargerID); got != "OCCUPIED" {
			t.Fatalf("charger status = %s, want OCCUPIED: a receipt already confirmed this charge", got)
		}
	})
}

// The STOP recovery path: an order the device would not stop is re-sent a STOP_CHARGING with a NEW
// command id, bounded, spaced by the backoff, and finally handed to an operator - and it is never
// released or settled on the platform's own initiative.
func TestStopRecoveryReissuesBoundedCommandsAndEscalates(t *testing.T) {
	db, ctx := integrationDB(t)
	fixture := newReceiptFixture(t, db, ctx)
	if _, err := fixture.store.ConfirmStart(ctx, order.ConfirmStartCommand{
		OrderNo: fixture.orderNo, ChargerID: fixture.chargerID, OccurredAt: time.Now().UTC().Add(-time.Minute),
		EventID: "evt_recovery_start_" + fixture.orderNo, RequestHash: "d-" + fixture.orderNo, TraceID: "trace",
	}); err != nil {
		t.Fatalf("ConfirmStart() error = %v", err)
	}
	if _, err := fixture.store.StopCharging(ctx, order.TransitionCommand{
		UserID: fixture.userID, OrderNo: fixture.orderNo,
		IdempotencyKey: "recovery-stop", RequestHash: "h", TraceID: "trace",
	}); err != nil {
		t.Fatalf("StopCharging() error = %v", err)
	}
	// The device refuses the stop: the order stays STOPPING and the charger is parked in FAULT.
	if _, err := fixture.store.RecordChargerCommandResult(ctx, "cmd_recovery_1_"+fixture.orderNo, fixture.orderNo,
		fixture.chargerID, order.CommandStopCharging, "FAILED", "trace"); err != nil {
		t.Fatalf("RecordChargerCommandResult() error = %v", err)
	}
	if got := orderStatus(t, db, ctx, fixture.orderNo); got != order.StatusStopping {
		t.Fatalf("status = %s, want STOPPING", got)
	}

	stopCommands := func() []map[string]string {
		t.Helper()
		return chargerCommandsWithAction(t, db, ctx, fixture.orderNo, order.CommandStopCharging)
	}
	before := stopCommands()
	if len(before) != 1 {
		t.Fatalf("expected the user's stop to have queued one command, got %v", before)
	}

	policy := order.StopRecoveryPolicy{MaxAttempts: 2, Backoff: 10 * time.Minute}
	now := time.Now().UTC()

	// Inside the backoff window nothing is re-sent: a device that just refused a command must not
	// be hammered.
	storeClock := now
	fixture.store.clock = func() time.Time { return storeClock }
	first, err := fixture.store.ReissueStopCommands(ctx, policy)
	if err != nil {
		t.Fatalf("ReissueStopCommands() error = %v", err)
	}
	// The sweep is global - it walks every stuck order - so the assertions are about this order's
	// commands, not about the counters, which other stuck orders in the database also move.
	if len(stopCommands()) != 1 {
		t.Fatalf("inside the backoff the sweep sent another command: %v (result %+v)", stopCommands(), first)
	}

	// After the backoff the sweep re-sends the stop with a NEW command id: reusing the first id
	// would be answered from the gateway's stored outcome, so the device would never see the retry.
	storeClock = now.Add(11 * time.Minute)
	second, err := fixture.store.ReissueStopCommands(ctx, policy)
	if err != nil {
		t.Fatalf("ReissueStopCommands() error = %v", err)
	}
	if second.Reissued < 1 {
		t.Fatalf("expected the sweep to reissue a stop command, got %+v", second)
	}
	after := stopCommands()
	if len(after) != 2 {
		t.Fatalf("expected two stop commands in total, got %v", after)
	}
	if after[0]["command_id"] == after[1]["command_id"] {
		t.Fatalf("the recovery reused the command id %q", after[0]["command_id"])
	}
	if after[1]["action"] != order.CommandStopCharging || after[1]["order_no"] != fixture.orderNo {
		t.Fatalf("unexpected recovery command %v", after[1])
	}

	// The attempt limit counts every STOP_CHARGING sent for the order, so the next sweep is the
	// last one that may act: it escalates instead of sending a third command.
	storeClock = now.Add(22 * time.Minute)
	third, err := fixture.store.ReissueStopCommands(ctx, policy)
	if err != nil {
		t.Fatalf("ReissueStopCommands() error = %v", err)
	}
	if got := len(stopCommands()); got != 2 {
		t.Fatalf("the sweep sent %d stop commands, want the bounded 2", got)
	}
	if audit := auditRows(t, db, ctx, fixture.orderNo, "STOP_RECOVERY_ESCALATED"); audit != 1 {
		t.Fatalf("expected one escalation row for this order, got %d (result %+v)", audit, third)
	}

	// The escalation is written once: repeating it every sweep would drown the alert it exists for.
	if _, err := fixture.store.ReissueStopCommands(ctx, policy); err != nil {
		t.Fatalf("ReissueStopCommands() error = %v", err)
	}
	if audit := auditRows(t, db, ctx, fixture.orderNo, "STOP_RECOVERY_ESCALATED"); audit != 1 {
		t.Fatalf("the escalation must be recorded once, got %d rows", audit)
	}
	if got := len(stopCommands()); got != 2 {
		t.Fatalf("a sweep past the limit sent another command: %d", got)
	}

	// And the order is exactly where it was: STOPPING, charger FAULT, nothing released, nothing
	// settled. The device may still be charging, so any automatic close would be a wrong bill or a
	// broken device back in the pool.
	if got := orderStatus(t, db, ctx, fixture.orderNo); got != order.StatusStopping {
		t.Fatalf("status = %s, want STOPPING", got)
	}
	if got := chargerStatus(t, db, ctx, fixture.chargerID); got != "FAULT" {
		t.Fatalf("charger status = %s, want FAULT", got)
	}
	var (
		energy   int64
		paidCent int64
		payment  string
	)
	if err := db.QueryRowContext(ctx,
		`SELECT energy_wh, paid_cents, payment_status FROM charging_orders WHERE order_no = $1`, fixture.orderNo).
		Scan(&energy, &paidCent, &payment); err != nil {
		t.Fatalf("read order: %v", err)
	}
	if energy != 0 || paidCent != 0 || payment == "PAID" {
		t.Fatalf("recovery closed the order: energy=%d paid=%d payment=%s", energy, paidCent, payment)
	}
}

// A STOP failure that is later resolved by a real receipt still completes the order normally: the
// recovery path only exists to get an explicit stop out of the device, and the receipt remains the
// only thing that completes an order.
func TestStopRecoveryEndsWithAReceipt(t *testing.T) {
	db, ctx := integrationDB(t)
	fixture := newReceiptFixture(t, db, ctx)
	startedAt := time.Now().UTC().Add(-30 * time.Minute).Truncate(time.Millisecond)

	if _, err := fixture.store.ConfirmStart(ctx, order.ConfirmStartCommand{
		OrderNo: fixture.orderNo, ChargerID: fixture.chargerID, OccurredAt: startedAt,
		EventID: "evt_resolve_start_" + fixture.orderNo, RequestHash: "d-" + fixture.orderNo, TraceID: "trace",
	}); err != nil {
		t.Fatalf("ConfirmStart() error = %v", err)
	}
	if _, err := fixture.store.StopCharging(ctx, order.TransitionCommand{
		UserID: fixture.userID, OrderNo: fixture.orderNo,
		IdempotencyKey: "resolve-stop", RequestHash: "h", TraceID: "trace",
	}); err != nil {
		t.Fatalf("StopCharging() error = %v", err)
	}
	if _, err := fixture.store.RecordChargerCommandResult(ctx, "cmd_resolve_1_"+fixture.orderNo, fixture.orderNo,
		fixture.chargerID, order.CommandStopCharging, "FAILED", "trace"); err != nil {
		t.Fatalf("RecordChargerCommandResult() error = %v", err)
	}
	if _, err := fixture.store.ReissueStopCommands(ctx, order.StopRecoveryPolicy{MaxAttempts: 3, Backoff: time.Nanosecond}); err != nil {
		t.Fatalf("ReissueStopCommands() error = %v", err)
	}
	if got := chargerStatus(t, db, ctx, fixture.chargerID); got != "FAULT" {
		t.Fatalf("charger status = %s, want FAULT before the receipt", got)
	}

	// The retried stop finally works on the device, which then reports the stop.
	completed, err := fixture.store.ConfirmStop(ctx, order.ConfirmStopCommand{
		OrderNo: fixture.orderNo, ChargerID: fixture.chargerID, EnergyWh: 1000,
		OccurredAt: startedAt.Add(20 * time.Minute),
		EventID:    "evt_resolve_stop_" + fixture.orderNo, RequestHash: "d-" + fixture.orderNo, TraceID: "trace",
	})
	if err != nil {
		t.Fatalf("ConfirmStop() error = %v", err)
	}
	if completed.Status != order.StatusCompleted {
		t.Fatalf("status = %s, want COMPLETED", completed.Status)
	}
	if got := chargerStatus(t, db, ctx, fixture.chargerID); got != "IDLE" {
		t.Fatalf("charger status = %s, want IDLE after the device reported the stop", got)
	}
}
