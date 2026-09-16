package postgres

import (
	"testing"
	"time"

	"github.com/heguangV/charging-station-platform/backend/internal/order"
)

// The running meter is what the app shows during a charge. A reading updates the
// order's metered columns, and the detail derives the accrued amount with the same
// time-of-use engine the final bill uses, so the number the customer watches
// converges on the invoice instead of drifting away from it.
func TestProgressReadingDrivesTheLiveAmount(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewOrderStore(db)
	if err != nil {
		t.Fatalf("NewOrderStore() error = %v", err)
	}
	fixture := newReceiptFixture(t, db, ctx)
	startedAt := time.Now().UTC().Add(-30 * time.Minute).Truncate(time.Second)

	if _, err := fixture.store.ConfirmStart(ctx, order.ConfirmStartCommand{
		OrderNo: fixture.orderNo, ChargerID: fixture.chargerID, OccurredAt: startedAt,
		EventID: "progress_start_" + fixture.orderNo, RequestHash: "h", TraceID: "trace",
	}); err != nil {
		t.Fatalf("ConfirmStart() error = %v", err)
	}

	// A flat tariff keeps the expectation readable: 120 cents/kWh + 50 service.
	if _, err := db.ExecContext(ctx, `UPDATE charging_orders
SET price_per_kwh_cents = 120, service_price_per_kwh_cents = 50,
    off_peak_price_per_kwh_cents = NULL, off_peak_start_hour = NULL, off_peak_end_hour = NULL
WHERE order_no = $1`, fixture.orderNo); err != nil {
		t.Fatalf("configure tariff snapshot: %v", err)
	}

	readingAt := startedAt.Add(10 * time.Minute)
	applied, err := store.ConfirmProgress(ctx, order.ConfirmProgressCommand{
		OrderNo: fixture.orderNo, ChargerID: fixture.chargerID, EnergyWh: 2000, OccurredAt: readingAt,
		EventID: "progress_1_" + fixture.orderNo, RequestHash: "h", TraceID: "trace",
	})
	if err != nil {
		t.Fatalf("ConfirmProgress() error = %v", err)
	}
	if applied.MeteredEnergyWh != nil {
		t.Fatalf("the command result already carries the live meter (%v); it is a read-time figure",
			applied.MeteredEnergyWh)
	}

	detail, err := store.GetOrderByNo(ctx, fixture.userID, fixture.orderNo)
	if err != nil {
		t.Fatalf("GetOrderByNo() error = %v", err)
	}
	if detail.MeteredEnergyWh == nil || *detail.MeteredEnergyWh != 2000 {
		t.Fatalf("meteredEnergyWh = %v, want 2000", detail.MeteredEnergyWh)
	}
	if detail.MeteredAt == nil || !detail.MeteredAt.Equal(readingAt) {
		t.Fatalf("meteredAt = %v, want the reading's fact time %v", detail.MeteredAt, readingAt)
	}
	// 2 kWh at 120 + 50 cents/kWh = 340 cents.
	if detail.MeteredAmountCent == nil || *detail.MeteredAmountCent != 340 {
		t.Fatalf("meteredAmountCent = %v, want 340 (2 kWh at 120+50)", detail.MeteredAmountCent)
	}
	// The settled figures stay untouched while the charge runs.
	if detail.AmountCent != 0 || detail.EnergyWh != 0 {
		t.Fatalf("settled figures moved during a charge: amount=%d energy=%d", detail.AmountCent, detail.EnergyWh)
	}

	// A higher reading moves the meter forward.
	if _, err := store.ConfirmProgress(ctx, order.ConfirmProgressCommand{
		OrderNo: fixture.orderNo, ChargerID: fixture.chargerID, EnergyWh: 3000,
		OccurredAt: readingAt.Add(5 * time.Minute),
		EventID:    "progress_2_" + fixture.orderNo, RequestHash: "h", TraceID: "trace",
	}); err != nil {
		t.Fatalf("ConfirmProgress() (second) error = %v", err)
	}
	detail, err = store.GetOrderByNo(ctx, fixture.userID, fixture.orderNo)
	if err != nil {
		t.Fatalf("GetOrderByNo() error = %v", err)
	}
	if detail.MeteredEnergyWh == nil || *detail.MeteredEnergyWh != 3000 {
		t.Fatalf("meteredEnergyWh after the second reading = %v, want 3000", detail.MeteredEnergyWh)
	}
	if detail.MeteredAmountCent == nil || *detail.MeteredAmountCent != 510 {
		t.Fatalf("meteredAmountCent = %v, want 510 (3 kWh at 120+50)", detail.MeteredAmountCent)
	}

	// A stale delivery (an older fact the gateway retried) must never lower what
	// the customer sees, and must not be an error either.
	stale, err := store.ConfirmProgress(ctx, order.ConfirmProgressCommand{
		OrderNo: fixture.orderNo, ChargerID: fixture.chargerID, EnergyWh: 500,
		OccurredAt: readingAt,
		EventID:    "progress_stale_" + fixture.orderNo, RequestHash: "h", TraceID: "trace",
	})
	if err != nil {
		t.Fatalf("stale reading error = %v, want a no-op success", err)
	}
	if stale.OrderNo != fixture.orderNo {
		t.Fatalf("stale reading returned %#v, want the current order", stale)
	}
	after, err := store.GetOrderByNo(ctx, fixture.userID, fixture.orderNo)
	if err != nil {
		t.Fatalf("GetOrderByNo() error = %v", err)
	}
	if after.MeteredEnergyWh == nil || *after.MeteredEnergyWh != 3000 {
		t.Fatalf("meteredEnergyWh after a stale reading = %v, want 3000 (never goes backwards)", after.MeteredEnergyWh)
	}

	// Settlement replaces the live figures: after the stop the order carries the
	// billed energy and amount, and no live meter.
	if _, err := fixture.store.StopCharging(ctx, order.TransitionCommand{
		UserID: fixture.userID, OrderNo: fixture.orderNo,
		IdempotencyKey: "progress_stop_" + fixture.orderNo, RequestHash: "h", TraceID: "trace",
	}); err != nil {
		t.Fatalf("StopCharging() error = %v", err)
	}
	if _, err := fixture.store.ConfirmStop(ctx, order.ConfirmStopCommand{
		OrderNo: fixture.orderNo, ChargerID: fixture.chargerID, EnergyWh: 3200,
		OccurredAt: readingAt.Add(10 * time.Minute),
		EventID:    "progress_stop_receipt_" + fixture.orderNo, RequestHash: "h", TraceID: "trace",
	}); err != nil {
		t.Fatalf("ConfirmStop() error = %v", err)
	}
	settled, err := store.GetOrderByNo(ctx, fixture.userID, fixture.orderNo)
	if err != nil {
		t.Fatalf("GetOrderByNo() error = %v", err)
	}
	if settled.Status != order.StatusCompleted {
		t.Fatalf("status = %s, want COMPLETED", settled.Status)
	}
	if settled.EnergyWh != 3200 {
		t.Fatalf("settled energyWh = %d, want the final reading 3200", settled.EnergyWh)
	}
	if settled.AmountCent != order.ComputeTOUBill(3200, startedAt,
		readingAt.Add(10*time.Minute), order.TariffConfig{PeakPrice: 120, ServicePrice: 50}).AmountCent {
		t.Fatalf("settled amount = %d, want the same engine's figure", settled.AmountCent)
	}
	if settled.MeteredEnergyWh != nil || settled.MeteredAmountCent != nil || settled.MeteredAt != nil {
		t.Fatalf("a completed order still carries live meter fields: %#v", settled)
	}
}

// A reading only means something while the charger is charging. Anything else is
// either a very late delivery or a gateway that lost track of the order, and it is
// refused with an audit row rather than silently stored.
func TestProgressReadingRequiresARunningCharge(t *testing.T) {
	db, ctx := integrationDB(t)
	fixture := newReceiptFixture(t, db, ctx) // CREATED -> STARTING, not charging yet

	_, err := fixture.store.ConfirmProgress(ctx, order.ConfirmProgressCommand{
		OrderNo: fixture.orderNo, ChargerID: fixture.chargerID, EnergyWh: 700,
		OccurredAt: time.Now().UTC(),
		EventID:    "progress_wrong_state_" + fixture.orderNo, RequestHash: "h", TraceID: "trace",
	})
	if err == nil {
		t.Fatal("progress on an order that is not CHARGING was accepted")
	}

	var rejected int64
	if err := db.QueryRowContext(ctx, `SELECT count(*) FROM order_events e
JOIN charging_orders o ON o.id = e.order_id
WHERE o.order_no = $1 AND e.event_type = 'CHARGER_EVENT_REJECTED'
  AND e.payload->>'eventType' = 'CHARGE_PROGRESS'`, fixture.orderNo).Scan(&rejected); err != nil {
		t.Fatalf("count rejections: %v", err)
	}
	if rejected != 1 {
		t.Fatalf("progress rejections recorded = %d, want 1", rejected)
	}

	var metered int64
	if err := db.QueryRowContext(ctx, `SELECT metered_energy_wh FROM charging_orders WHERE order_no = $1`,
		fixture.orderNo).Scan(&metered); err != nil {
		t.Fatalf("read metered energy: %v", err)
	}
	if metered != 0 {
		t.Fatalf("metered energy = %d, want 0: a refused reading must not be stored", metered)
	}
}

// A reading that predates the recorded start is not a reading of this charge.
func TestProgressReadingCannotPredateTheStart(t *testing.T) {
	db, ctx := integrationDB(t)
	fixture := newReceiptFixture(t, db, ctx)
	startedAt := time.Now().UTC().Add(-5 * time.Minute).Truncate(time.Second)
	if _, err := fixture.store.ConfirmStart(ctx, order.ConfirmStartCommand{
		OrderNo: fixture.orderNo, ChargerID: fixture.chargerID, OccurredAt: startedAt,
		EventID: "predate_start_" + fixture.orderNo, RequestHash: "h", TraceID: "trace",
	}); err != nil {
		t.Fatalf("ConfirmStart() error = %v", err)
	}

	_, err := fixture.store.ConfirmProgress(ctx, order.ConfirmProgressCommand{
		OrderNo: fixture.orderNo, ChargerID: fixture.chargerID, EnergyWh: 400,
		OccurredAt: startedAt.Add(-time.Minute),
		EventID:    "predate_reading_" + fixture.orderNo, RequestHash: "h", TraceID: "trace",
	})
	if err == nil {
		t.Fatal("a reading before the start of the charge was accepted")
	}
}
