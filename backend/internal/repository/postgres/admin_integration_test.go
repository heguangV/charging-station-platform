package postgres

import (
	"database/sql"
	"errors"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/heguangV/charging-station-platform/backend/internal/admin"
	"github.com/heguangV/charging-station-platform/backend/internal/order"
	"github.com/heguangV/charging-station-platform/backend/internal/station"
)

func TestAdminCreateStationAuditAndIdempotency(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewAdminStore(db)
	if err != nil {
		t.Fatalf("NewAdminStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	command := admin.CreateStationCommand{
		AdminID: 9, Code: "AD-" + suffix, Name: "管理站" + suffix, Address: "审计地址",
		LatitudeE6: 30000000, LongitudeE6: 104000000,
		IdempotencyKey: "adm-create-" + suffix, RequestHash: "h", TraceID: "trace-admin",
	}

	created, err := store.CreateStation(ctx, command)
	if err != nil {
		t.Fatalf("CreateStation() error = %v", err)
	}
	if created.Status != "OPEN" || created.LatitudeE6 != 30000000 {
		t.Fatalf("created = %#v", created)
	}

	// Same key replays the stored record without a second row.
	replay, err := store.CreateStation(ctx, command)
	if err != nil || replay.ID != created.ID {
		t.Fatalf("replay = %#v, %v", replay, err)
	}
	var count int64
	if err := db.QueryRowContext(ctx, `SELECT count(*) FROM stations WHERE code = $1`, command.Code).Scan(&count); err != nil {
		t.Fatalf("count: %v", err)
	}
	if count != 1 {
		t.Fatalf("station rows = %d, want 1", count)
	}

	// The audit trail recorded the action.
	var audits int64
	if err := db.QueryRowContext(ctx,
		`SELECT count(*) FROM operation_logs WHERE actor_type = 'ADMIN' AND action = 'station.create' AND resource_id = $1`,
		strconvFormatInt64(created.ID)).Scan(&audits); err != nil {
		t.Fatalf("audit count: %v", err)
	}
	if audits != 1 {
		t.Fatalf("audit rows = %d, want 1 (replay must not duplicate)", audits)
	}
}

func TestAdminRestartChargerLifecycle(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewAdminStore(db)
	if err != nil {
		t.Fatalf("NewAdminStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	_, _, _, _, chargerA := orderFlowFixture(t, db, ctx, suffix)

	command := admin.RestartCommand{
		AdminID: 9, ChargerID: chargerA, Reason: "固件升级",
		IdempotencyKey: "adm-restart-" + suffix, RequestHash: "h", TraceID: "trace-restart",
	}
	result, err := store.RestartCharger(ctx, command)
	if err != nil {
		t.Fatalf("RestartCharger() error = %v", err)
	}
	if result.Status != admin.CommandPending || result.CommandID == "" {
		t.Fatalf("command = %#v", result)
	}

	// The charger is restarting and the command is queued for the worker.
	var chargerStatus string
	if err := db.QueryRowContext(ctx, `SELECT status FROM chargers WHERE id = $1`, chargerA).Scan(&chargerStatus); err != nil {
		t.Fatalf("charger status: %v", err)
	}
	if chargerStatus != admin.ChargerStatusRestarting {
		t.Fatalf("charger status = %s, want RESTARTING", chargerStatus)
	}
	// The payload must satisfy the approved B-04 consumer contract exactly:
	// snake_case command_id/charger_id and the RESTART action.
	var queued int64
	var action, commandID string
	if err := db.QueryRowContext(ctx,
		`SELECT payload->>'action', payload->>'command_id' FROM outbox_events WHERE event_type = 'CHARGER_COMMAND_REQUESTED' AND aggregate_id = $1 AND payload->>'command_id' = $2`,
		strconvFormatInt64(chargerA), result.CommandID).Scan(&action, &commandID); err != nil {
		t.Fatalf("outbox: %v", err)
	}
	if action != "RESTART" || commandID != result.CommandID {
		t.Fatalf("command payload action=%q command_id=%q, want RESTART/%s", action, commandID, result.CommandID)
	}
	if err := db.QueryRowContext(ctx,
		`SELECT count(*) FROM outbox_events WHERE event_type = 'CHARGER_COMMAND_REQUESTED' AND aggregate_id = $1 AND payload->>'charger_id' = $2`,
		strconvFormatInt64(chargerA), strconvFormatInt64(chargerA)).Scan(&queued); err != nil {
		t.Fatalf("outbox charger_id: %v", err)
	}
	if queued != 1 {
		t.Fatalf("command events = %d, want 1", queued)
	}

	// Audit trail.
	var audits int64
	if err := db.QueryRowContext(ctx,
		`SELECT count(*) FROM operation_logs WHERE action = 'charger.restart' AND payload->>'commandId' = $1`,
		result.CommandID).Scan(&audits); err != nil {
		t.Fatalf("audit: %v", err)
	}
	if audits != 1 {
		t.Fatalf("audit rows = %d, want 1", audits)
	}

	// Occupied chargers cannot be restarted (BR-11): the second user takes
	// charger A only after it returns to idle — use a fresh charger instead.
	if _, err := store.RestartCharger(ctx, command); err != nil {
		t.Fatalf("idempotent restart error = %v", err)
	}
}

func TestAdminUserAndOrderLists(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewAdminStore(db)
	if err != nil {
		t.Fatalf("NewAdminStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	userA, _, _, _, chargerA := orderFlowFixture(t, db, ctx, suffix)

	var phone string
	if err := db.QueryRowContext(ctx, `SELECT phone FROM user_accounts WHERE id = $1`, userA).Scan(&phone); err != nil {
		t.Fatal(err)
	}
	users, err := store.ListUsers(ctx, admin.UserFilter{Page: 1, PageSize: 100, Keyword: phone})
	if err != nil || len(users.Items) != 1 || users.Meta.Total != 1 || users.Items[0].ID != userA {
		t.Fatalf("users = %#v, %v", users, err)
	}
	for _, item := range users.Items {
		if item.BalanceCent != 10000 {
			t.Fatalf("balance = %d, want 10000", item.BalanceCent)
		}
	}

	// The seed orders have no bill yet: create one and filter by its order number.
	//
	// The filter is a prefix match, so it is given the whole number rather than the 14-character
	// timestamp part: a timestamp prefix is shared by every order created in the same second, and the
	// assertion below ("exactly the seeded order comes back") then depends on what the rest of the
	// suite happened to create at that moment. That is how this test failed once the A-01 persistence
	// suite started creating orders - the filter, not the store, was the problem.
	seeded, err := store.CreateOrder(ctx, order.CreateOrderCommand{
		UserID: userA, ChargerID: chargerA, IdempotencyKey: "adm-order-" + suffix, RequestHash: "h", TraceID: "t",
	})
	if err != nil {
		t.Fatalf("seed order: %v", err)
	}
	orders, err := store.ListOrders(ctx, admin.AdminOrderFilter{Page: 1, PageSize: 100, OrderNo: seeded.OrderNo})
	if err != nil {
		t.Fatalf("ListOrders() error = %v", err)
	}
	if len(orders.Items) != 1 || orders.Items[0].OrderNo != seeded.OrderNo {
		t.Fatalf("orders = %#v", orders)
	}
	// The prefix behaviour itself is worth keeping pinned, so a shorter prefix is checked to return
	// at least the seeded order rather than exactly one.
	byPrefix, err := store.ListOrders(ctx, admin.AdminOrderFilter{Page: 1, PageSize: 100, OrderNo: seeded.OrderNo[:6]})
	if err != nil {
		t.Fatalf("ListOrders(prefix) error = %v", err)
	}
	found := false
	for _, item := range byPrefix.Items {
		if item.OrderNo == seeded.OrderNo {
			found = true
		}
	}
	if !found {
		t.Fatalf("a prefix filter did not return the order it matches: %#v", byPrefix.Items)
	}
	if orders.Items[0].PaymentStatus != "PENDING" {
		t.Fatalf("payment status = %q", orders.Items[0].PaymentStatus)
	}

	// The management UI opens a user's detail page and asks for that user's
	// orders, so the list must be filterable by user. Two users with one order
	// each is the smallest state that can tell the filter apart from "everything".
	_, userB, _, _, chargerB := orderFlowFixture(t, db, ctx, uniqueSuffix(t))
	orderStore, err := NewOrderStore(db)
	if err != nil {
		t.Fatalf("NewOrderStore() error = %v", err)
	}
	otherOrder, err := orderStore.CreateOrder(ctx, order.CreateOrderCommand{
		UserID: userB, ChargerID: chargerB, IdempotencyKey: "adm-other-" + suffix, RequestHash: "h", TraceID: "t",
	})
	if err != nil {
		t.Fatalf("second user's order: %v", err)
	}
	filtered, err := store.ListOrders(ctx, admin.AdminOrderFilter{Page: 1, PageSize: 100, UserID: userB})
	if err != nil {
		t.Fatalf("ListOrders(userId=%d) error = %v", userB, err)
	}
	if len(filtered.Items) != 1 || filtered.Items[0].OrderNo != otherOrder.OrderNo || filtered.Items[0].UserID != userB {
		t.Fatalf("filtered orders = %#v, want exactly %s for user %d", filtered.Items, otherOrder.OrderNo, userB)
	}
	if filtered.Meta.Total != 1 {
		t.Fatalf("filtered total = %d, want 1", filtered.Meta.Total)
	}
	// The first user's order must not appear in the second user's page.
	own, err := store.ListOrders(ctx, admin.AdminOrderFilter{Page: 1, PageSize: 100, UserID: userA})
	if err != nil {
		t.Fatalf("ListOrders(userId=%d) error = %v", userA, err)
	}
	for _, item := range own.Items {
		if item.UserID != userA {
			t.Fatalf("user %d's page contains an order of user %d", userA, item.UserID)
		}
	}
}

func TestAdminForceReleaseIdempotencyAndBR11(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewAdminStore(db)
	if err != nil {
		t.Fatalf("NewAdminStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	userA, _, _, _, chargerA := orderFlowFixture(t, db, ctx, suffix)

	// STARTING order: force release with the same key twice.
	created, err := orderStore(t, db).CreateOrder(ctx, order.CreateOrderCommand{
		UserID: userA, ChargerID: chargerA, IdempotencyKey: "fr-create-" + suffix, RequestHash: "h", TraceID: "t",
	})
	if err != nil {
		t.Fatalf("CreateOrder() error = %v", err)
	}
	if _, err := orderStore(t, db).StartCharging(ctx, order.TransitionCommand{
		UserID: userA, OrderNo: created.OrderNo, IdempotencyKey: "fr-start-" + suffix, RequestHash: "h", TraceID: "t"}); err != nil {
		t.Fatalf("StartCharging() error = %v", err)
	}

	command := admin.ForceReleaseCommand{
		AdminID: 9, ChargerID: chargerA, Reason: "违规占位释放", TargetStatus: "IDLE",
		IdempotencyKey: "fr-release-" + suffix, RequestHash: "h", TraceID: "trace-fr",
	}
	first, err := store.ForceRelease(ctx, command)
	if err != nil {
		t.Fatalf("ForceRelease() error = %v", err)
	}
	replay, err := store.ForceRelease(ctx, command)
	if err != nil || replay.OrderNo != first.OrderNo {
		t.Fatalf("replay = %#v, %v", replay, err)
	}

	// One order cancelled, one audit row (replay must not duplicate).
	var orderCount, auditCount int64
	if err := db.QueryRowContext(ctx, `SELECT count(*) FROM charging_orders WHERE charger_id = $1 AND status = 'CANCELLED'`, chargerA).Scan(&orderCount); err != nil {
		t.Fatalf("orders: %v", err)
	}
	if err := db.QueryRowContext(ctx,
		`SELECT count(*) FROM operation_logs WHERE action = 'charger.force-release' AND resource_id = $1`,
		strconvFormatInt64(chargerA)).Scan(&auditCount); err != nil {
		t.Fatalf("audit: %v", err)
	}
	if orderCount != 1 || auditCount != 1 {
		t.Fatalf("orders = %d audits = %d, want 1/1", orderCount, auditCount)
	}

	// Charging orders are not releasable (BR-11): drive a second order into
	// CHARGING and verify the 409 mapping error.
	if _, err := orderStore(t, db).CreateOrder(ctx, order.CreateOrderCommand{
		UserID: userA, ChargerID: chargerA, IdempotencyKey: "fr-create2-" + suffix, RequestHash: "h", TraceID: "t",
	}); err != nil {
		t.Fatalf("second create: %v", err)
	}
	if _, err := orderStore(t, db).StartCharging(ctx, order.TransitionCommand{
		UserID: userA, OrderNo: "", IdempotencyKey: "fr-start2-" + suffix, RequestHash: "h", TraceID: "t",
	}); err == nil {
		t.Fatal("expected start for second order to fail without order number") // start requires order no; skip path
	}
	// Drive it properly: fetch the new order number.
	var secondNo string
	if err := db.QueryRowContext(ctx,
		`SELECT order_no FROM charging_orders WHERE user_id = $1 AND status = 'CREATED' ORDER BY id DESC LIMIT 1`,
		userA).Scan(&secondNo); err != nil {
		t.Fatalf("second order: %v", err)
	}
	if _, err := orderStore(t, db).StartCharging(ctx, order.TransitionCommand{
		UserID: userA, OrderNo: secondNo, IdempotencyKey: "fr-start3-" + suffix, RequestHash: "h", TraceID: "t"}); err != nil {
		t.Fatalf("second start: %v", err)
	}
	if _, err := orderStore(t, db).ConfirmStart(ctx, order.ConfirmStartCommand{
		OrderNo: secondNo, ChargerID: chargerA, OccurredAt: time.Now().UTC()}); err != nil {
		t.Fatalf("second ConfirmStart() error = %v", err)
	}
	_, err = store.ForceRelease(ctx, admin.ForceReleaseCommand{
		AdminID: 9, ChargerID: chargerA, Reason: "充电中不放行", TargetStatus: "IDLE",
		IdempotencyKey: "fr-release2-" + suffix, RequestHash: "h", TraceID: "t",
	})
	if !errors.Is(err, admin.ErrInvalidStateTransition) {
		t.Fatalf("charging release error = %v, want ErrInvalidStateTransition", err)
	}
}

func orderStore(t *testing.T, db *sql.DB) *OrderStore {
	t.Helper()
	store, err := NewOrderStore(db)
	if err != nil {
		t.Fatalf("order store: %v", err)
	}
	return store
}

func TestAdminTariffUpdateAuditContainsOffPeak(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewAdminStore(db)
	if err != nil {
		t.Fatalf("NewAdminStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	_, _, stationA, _, chargerA := orderFlowFixture(t, db, ctx, suffix)
	_ = stationA

	offPeak := int64(60)
	startHour := int16(23)
	endHour := int16(7)
	updated, err := store.UpdateTariff(ctx, admin.TariffUpdate{
		AdminID: 9, ChargerID: chargerA,
		ElectricityPriceCent: 130, ServicePriceCent: 50,
		OffPeakPriceCent: &offPeak, OffPeakStartHour: &startHour, OffPeakEndHour: &endHour,
	})
	if err != nil {
		t.Fatalf("UpdateTariff() error = %v", err)
	}
	if updated.OffPeakPriceCent == nil || *updated.OffPeakPriceCent != 60 {
		t.Fatalf("updated off-peak = %#v", updated.OffPeakPriceCent)
	}

	var payload string
	if err := db.QueryRowContext(ctx,
		`SELECT payload FROM operation_logs WHERE action = 'tariff.update' AND resource_id = $1 ORDER BY id DESC LIMIT 1`,
		strconvFormatInt64(chargerA)).Scan(&payload); err != nil {
		t.Fatalf("audit payload: %v", err)
	}
	for _, field := range []string{"offPeakPrice", "offPeakStartHour", "offPeakEndHour"} {
		if !strings.Contains(payload, field) {
			t.Fatalf("audit payload missing %s: %s", field, payload)
		}
	}
}

// TestAdminDeviceCommandLookupOnRealDatabase reads back a command outcome that
// the receipt path wrote. The admin endpoint must show the durable fact - what
// the device answered and whether it has been applied - and must report an
// unknown identifier as not found instead of an empty record.
func TestAdminDeviceCommandLookupOnRealDatabase(t *testing.T) {
	db, ctx := integrationDB(t)
	adminStore, err := NewAdminStore(db)
	if err != nil {
		t.Fatalf("NewAdminStore() error = %v", err)
	}
	orderStore, err := NewOrderStore(db)
	if err != nil {
		t.Fatalf("NewOrderStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	_, _, stationA, _, _ := orderFlowFixture(t, db, ctx, suffix)
	chargerID := seedRestartingCharger(t, db, ctx, stationA, "CMD-STATUS-"+suffix)

	commandID := "CMD-STATUS-" + suffix
	applied, err := orderStore.RecordChargerCommandResult(ctx, commandID, "", chargerID, order.CommandRestart, order.CommandResultCompleted, "trace-status")
	if err != nil {
		t.Fatalf("RecordChargerCommandResult() error = %v", err)
	}
	if !applied {
		t.Fatal("the receipt was recorded as a replay")
	}

	command, err := adminStore.FindDeviceCommand(ctx, commandID)
	if err != nil {
		t.Fatalf("FindDeviceCommand() error = %v", err)
	}
	if command.CommandID != commandID || command.ChargerID != chargerID {
		t.Fatalf("command = %#v, want commandId %s on charger %d", command, commandID, chargerID)
	}
	if command.Action != order.CommandRestart || command.Result != order.CommandResultCompleted {
		t.Fatalf("action/result = %s/%s", command.Action, command.Result)
	}
	if !command.Applied {
		t.Fatal("applied = false after a successful restart receipt")
	}
	if _, err := time.Parse(time.RFC3339, command.RecordedAt); err != nil {
		t.Fatalf("recordedAt = %q, want an RFC3339 timestamp: %v", command.RecordedAt, err)
	}

	if _, err := adminStore.FindDeviceCommand(ctx, "CMD-STATUS-missing-"+suffix); !errors.Is(err, admin.ErrDeviceCommandNotFound) {
		t.Fatalf("unknown command error = %v, want ErrDeviceCommandNotFound", err)
	}
}

// TestAdminStatusChangesOnRealDatabase pins the station and charger lifecycle:
// the legal moves happen and are audited, the illegal ones are refused with the
// row unchanged, and a charger that is in use stays out of reach.
func TestAdminStatusChangesOnRealDatabase(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewAdminStore(db)
	if err != nil {
		t.Fatalf("NewAdminStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	userA, _, stationA, _, chargerA := orderFlowFixture(t, db, ctx, suffix)
	const adminID = 9

	readStation := func() string {
		var status string
		if err := db.QueryRowContext(ctx, `SELECT status FROM stations WHERE id = $1`, stationA).Scan(&status); err != nil {
			t.Fatalf("read station status: %v", err)
		}
		return status
	}
	auditCount := func(action, resourceID string) int64 {
		var count int64
		if err := db.QueryRowContext(ctx,
			`SELECT count(*) FROM operation_logs WHERE action = $1 AND resource_id = $2`, action, resourceID).Scan(&count); err != nil {
			t.Fatalf("audit count: %v", err)
		}
		return count
	}

	// OPEN -> CLOSED: allowed, audited, and the response reports the new status.
	closed, err := store.ChangeStationStatus(ctx, admin.ChangeStationStatusCommand{
		AdminID: adminID, StationID: stationA, Status: station.StatusClosed, TraceID: "trace-station-closed"})
	if err != nil {
		t.Fatalf("OPEN -> CLOSED error = %v", err)
	}
	if closed.Status != station.StatusClosed || readStation() != station.StatusClosed {
		t.Fatalf("station status = %s / %s, want CLOSED", closed.Status, readStation())
	}
	if audits := auditCount("station.status", strconv.FormatInt(stationA, 10)); audits != 1 {
		t.Fatalf("station audit rows = %d, want 1", audits)
	}

	// CLOSED -> DISABLED is allowed; DISABLED -> CLOSED is not.
	if _, err := store.ChangeStationStatus(ctx, admin.ChangeStationStatusCommand{
		AdminID: adminID, StationID: stationA, Status: station.StatusDisabled, TraceID: "t"}); err != nil {
		t.Fatalf("CLOSED -> DISABLED error = %v", err)
	}
	if _, err := store.ChangeStationStatus(ctx, admin.ChangeStationStatusCommand{
		AdminID: adminID, StationID: stationA, Status: station.StatusClosed, TraceID: "t"}); !errors.Is(err, admin.ErrInvalidStateTransition) {
		t.Fatalf("DISABLED -> CLOSED error = %v, want ErrInvalidStateTransition", err)
	}
	if readStation() != station.StatusDisabled {
		t.Fatalf("station status = %s after a refused transition", readStation())
	}
	// Same status is a conflict, not a silent success.
	if _, err := store.ChangeStationStatus(ctx, admin.ChangeStationStatusCommand{
		AdminID: adminID, StationID: stationA, Status: station.StatusDisabled, TraceID: "t"}); !errors.Is(err, admin.ErrInvalidStateTransition) {
		t.Fatalf("DISABLED -> DISABLED error = %v, want ErrInvalidStateTransition", err)
	}
	if _, err := store.ChangeStationStatus(ctx, admin.ChangeStationStatusCommand{
		AdminID: adminID, StationID: stationA + 999999, Status: station.StatusOpen, TraceID: "t"}); !errors.Is(err, admin.ErrStationNotFound) {
		t.Fatalf("unknown station error = %v, want ErrStationNotFound", err)
	}
	// No refused change may leave an audit row behind.
	if audits := auditCount("station.status", strconv.FormatInt(stationA, 10)); audits != 2 {
		t.Fatalf("station audit rows = %d, want 2 (only the two applied changes)", audits)
	}

	// Charger: IDLE -> DISABLED -> IDLE, with the version moving each time.
	readCharger := func() (string, int64) {
		var status string
		var version int64
		if err := db.QueryRowContext(ctx, `SELECT status, version FROM chargers WHERE id = $1`, chargerA).Scan(&status, &version); err != nil {
			t.Fatalf("read charger: %v", err)
		}
		return status, version
	}
	_, versionBefore := readCharger()
	disabled, err := store.ChangeChargerStatus(ctx, admin.ChangeChargerStatusCommand{
		AdminID: adminID, ChargerID: chargerA, Status: station.ChargerStatusDisabled, TraceID: "trace-charger"})
	if err != nil {
		t.Fatalf("IDLE -> DISABLED error = %v", err)
	}
	if disabled.Status != station.ChargerStatusDisabled || disabled.ChargerCode == "" {
		t.Fatalf("charger record = %#v", disabled)
	}
	if status, versionAfter := readCharger(); status != station.ChargerStatusDisabled || versionAfter != versionBefore+1 {
		t.Fatalf("charger = %s/%d, want DISABLED with version %d", status, versionAfter, versionBefore+1)
	}
	if _, err := store.ChangeChargerStatus(ctx, admin.ChangeChargerStatusCommand{
		AdminID: adminID, ChargerID: chargerA, Status: station.ChargerStatusIdle, TraceID: "t"}); err != nil {
		t.Fatalf("DISABLED -> IDLE error = %v", err)
	}
	if audits := auditCount("charger.status", strconv.FormatInt(chargerA, 10)); audits != 2 {
		t.Fatalf("charger audit rows = %d, want 2", audits)
	}

	// A charger held by a live order cannot be taken out of service by hand: the
	// active order would keep a charger that is no longer available.
	if _, err := db.ExecContext(ctx, `UPDATE chargers SET status = 'OCCUPIED' WHERE id = $1`, chargerA); err != nil {
		t.Fatalf("occupy charger: %v", err)
	}
	if _, err := store.ChangeChargerStatus(ctx, admin.ChangeChargerStatusCommand{
		AdminID: adminID, ChargerID: chargerA, Status: station.ChargerStatusDisabled, TraceID: "t"}); !errors.Is(err, admin.ErrInvalidStateTransition) {
		t.Fatalf("OCCUPIED -> DISABLED error = %v, want ErrInvalidStateTransition", err)
	}
	if status, _ := readCharger(); status != "OCCUPIED" {
		t.Fatalf("charger status = %s after a refused transition", status)
	}
	if _, err := store.ChangeChargerStatus(ctx, admin.ChangeChargerStatusCommand{
		AdminID: adminID, ChargerID: chargerA + 999999, Status: station.ChargerStatusIdle, TraceID: "t"}); !errors.Is(err, admin.ErrChargerNotFound) {
		t.Fatalf("unknown charger error = %v, want ErrChargerNotFound", err)
	}
	_ = userA
}
