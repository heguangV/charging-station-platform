package postgres

import (
	"testing"
	"time"

	"github.com/heguangV/charging-station-platform/backend/internal/order"
)

// The order detail carries the estimate basis a client needs while a charge is running: the
// device's fact time, the charger's rated power and the per-kWh price that applies right now.
// Without them a client cannot show anything but a placeholder, because the metered energy only
// arrives with the stop receipt.
func TestOrderDetailExposesEstimateBasisWhileCharging(t *testing.T) {
	db, ctx := integrationDB(t)
	fixture := newReceiptFixture(t, db, ctx)

	var powerWatt int64
	if err := db.QueryRowContext(ctx, `SELECT power_watt FROM chargers WHERE id = $1`, fixture.chargerID).
		Scan(&powerWatt); err != nil {
		t.Fatalf("read charger power: %v", err)
	}

	// A flat tariff first: the unit price is then independent of the wall clock.
	if _, err := db.ExecContext(ctx, `UPDATE chargers
   SET price_per_kwh_cents = 100, service_price_per_kwh_cents = 50,
       off_peak_electricity_price_per_kwh_cents = NULL, off_peak_start_hour = NULL, off_peak_end_hour = NULL
 WHERE id = $1`, fixture.chargerID); err != nil {
		t.Fatalf("configure flat tariff: %v", err)
	}
	// The snapshot is taken by StartCharging, so replay the CREATED -> STARTING transition.
	if _, err := db.ExecContext(ctx, `UPDATE charging_orders SET status = 'CREATED' WHERE order_no = $1`, fixture.orderNo); err != nil {
		t.Fatalf("reset order status: %v", err)
	}
	if _, err := fixture.store.StartCharging(ctx, order.TransitionCommand{
		UserID: fixture.userID, OrderNo: fixture.orderNo,
		IdempotencyKey: "estimate-start", RequestHash: "h", TraceID: "trace",
	}); err != nil {
		t.Fatalf("StartCharging() error = %v", err)
	}

	factTime := time.Now().UTC().Add(-10 * time.Minute).Truncate(time.Second)
	if _, err := fixture.store.ConfirmStart(ctx, order.ConfirmStartCommand{
		OrderNo: fixture.orderNo, ChargerID: fixture.chargerID, OccurredAt: factTime,
		EventID: "evt_estimate_start_" + fixture.orderNo, RequestHash: "d-" + fixture.orderNo, TraceID: "trace",
	}); err != nil {
		t.Fatalf("ConfirmStart() error = %v", err)
	}

	detail, err := fixture.store.GetOrderByNo(ctx, fixture.userID, fixture.orderNo)
	if err != nil {
		t.Fatalf("GetOrderByNo() error = %v", err)
	}
	if detail.Status != order.StatusCharging {
		t.Fatalf("status = %q, want CHARGING", detail.Status)
	}
	if detail.StartedAt == nil || !detail.StartedAt.Equal(factTime) {
		t.Fatalf("startedAt = %v, want the device fact time %v", detail.StartedAt, factTime)
	}
	if detail.ChargerPowerWatt == nil || *detail.ChargerPowerWatt != powerWatt {
		t.Fatalf("chargerPowerWatt = %v, want %d", detail.ChargerPowerWatt, powerWatt)
	}
	if detail.UnitPriceCentPerKwh == nil || *detail.UnitPriceCentPerKwh != 150 {
		t.Fatalf("unitPriceCentPerKwh = %v, want 150 (100 electricity + 50 service)", detail.UnitPriceCentPerKwh)
	}

	// A time-of-use snapshot resolves the window in the deployment's billing timezone: configure a
	// window that covers the current local hour (two hours wide, so the assertion cannot race an
	// hour boundary) and re-snapshot the order, then the unit price must be the off-peak one.
	billing := order.DefaultBillingLocation()
	hour := time.Now().In(billing).Hour()
	if _, err := db.ExecContext(ctx, `UPDATE chargers
   SET off_peak_electricity_price_per_kwh_cents = 60, off_peak_start_hour = $2, off_peak_end_hour = $3
 WHERE id = $1`, fixture.chargerID, hour, (hour+2)%24); err != nil {
		t.Fatalf("configure off-peak tariff: %v", err)
	}
	if _, err := db.ExecContext(ctx, `UPDATE charging_orders SET status = 'CREATED' WHERE order_no = $1`, fixture.orderNo); err != nil {
		t.Fatalf("reset order status: %v", err)
	}
	if _, err := fixture.store.StartCharging(ctx, order.TransitionCommand{
		UserID: fixture.userID, OrderNo: fixture.orderNo,
		IdempotencyKey: "estimate-resnapshot", RequestHash: "h", TraceID: "trace",
	}); err != nil {
		t.Fatalf("StartCharging() error = %v", err)
	}
	// Back to CHARGING, so the stop below is a legal transition.
	if _, err := fixture.store.ConfirmStart(ctx, order.ConfirmStartCommand{
		OrderNo: fixture.orderNo, ChargerID: fixture.chargerID, OccurredAt: factTime,
		EventID: "evt_estimate_start2_" + fixture.orderNo, RequestHash: "d2-" + fixture.orderNo, TraceID: "trace",
	}); err != nil {
		t.Fatalf("ConfirmStart() (second) error = %v", err)
	}
	offPeakDetail, err := fixture.store.GetOrderByNo(ctx, fixture.userID, fixture.orderNo)
	if err != nil {
		t.Fatalf("GetOrderByNo() error = %v", err)
	}
	if offPeakDetail.UnitPriceCentPerKwh == nil || *offPeakDetail.UnitPriceCentPerKwh != 110 {
		t.Fatalf("unitPriceCentPerKwh inside window %d-%d local = %v, want 110 (60 off-peak + 50 service); "+
			"a window read in UTC would report the peak 150",
			hour, (hour+2)%24, offPeakDetail.UnitPriceCentPerKwh)
	}

	// A finished order carries no estimate basis: the bill is known by then.
	if _, err := fixture.store.StopCharging(ctx, order.TransitionCommand{
		UserID: fixture.userID, OrderNo: fixture.orderNo,
		IdempotencyKey: "estimate-stop", RequestHash: "h", TraceID: "trace",
	}); err != nil {
		t.Fatalf("StopCharging() error = %v", err)
	}
	if _, err := fixture.store.ConfirmStop(ctx, order.ConfirmStopCommand{
		OrderNo: fixture.orderNo, ChargerID: fixture.chargerID, EnergyWh: 1000,
		OccurredAt: factTime.Add(30 * time.Minute),
		EventID:    "evt_estimate_stop_" + fixture.orderNo, RequestHash: "d-" + fixture.orderNo, TraceID: "trace",
	}); err != nil {
		t.Fatalf("ConfirmStop() error = %v", err)
	}
	completed, err := fixture.store.GetOrderByNo(ctx, fixture.userID, fixture.orderNo)
	if err != nil {
		t.Fatalf("GetOrderByNo() error = %v", err)
	}
	if completed.UnitPriceCentPerKwh != nil || completed.ChargerPowerWatt != nil {
		t.Fatalf("completed order still carries the estimate basis: price=%v power=%v",
			completed.UnitPriceCentPerKwh, completed.ChargerPowerWatt)
	}
	if completed.StartedAt == nil {
		t.Fatal("completed order lost startedAt")
	}
}

func ptrInt64(value int64) *int64 { return &value }

func ptrInt16(value int16) *int16 { return &value }
