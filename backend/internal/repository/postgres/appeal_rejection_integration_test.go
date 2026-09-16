package postgres

import (
	"context"
	"database/sql"
	"fmt"
	"testing"
	"time"

	"github.com/heguangV/charging-station-platform/backend/internal/order"
	"github.com/heguangV/charging-station-platform/backend/internal/review"
)

func appealAdminActor(t *testing.T, db *sql.DB, ctx context.Context) int64 {
	t.Helper()
	var adminID int64
	username := "appeal-reviewer-" + uniqueSuffix(t)
	if err := db.QueryRowContext(ctx, `INSERT INTO admin_accounts (username, password_hash, role)
VALUES ($1, 'integration-test-only', 'OPERATOR') RETURNING id`, username).Scan(&adminID); err != nil {
		t.Fatalf("seed appeal administrator: %v", err)
	}
	return adminID
}

func completeUnsettledOrderForAppeal(t *testing.T, ctx context.Context, store *OrderStore, suffix string, userID, chargerID int64) order.Order {
	t.Helper()
	created, err := store.CreateOrder(ctx, order.CreateOrderCommand{
		UserID: userID, ChargerID: chargerID, IdempotencyKey: "ap-create-" + suffix, RequestHash: "h", TraceID: "t",
	})
	if err != nil {
		t.Fatalf("CreateOrder() error = %v", err)
	}
	if _, err := store.StartCharging(ctx, order.TransitionCommand{
		UserID: userID, OrderNo: created.OrderNo, IdempotencyKey: "ap-start-" + suffix, RequestHash: "h", TraceID: "t",
	}); err != nil {
		t.Fatalf("StartCharging() error = %v", err)
	}
	startedAt := time.Now().UTC()
	if _, err := store.ConfirmStart(ctx, order.ConfirmStartCommand{
		OrderNo: created.OrderNo, ChargerID: chargerID, OccurredAt: startedAt,
	}); err != nil {
		t.Fatalf("ConfirmStart() error = %v", err)
	}
	if _, err := store.StopCharging(ctx, order.TransitionCommand{
		UserID: userID, OrderNo: created.OrderNo, IdempotencyKey: "ap-stop-" + suffix, RequestHash: "h", TraceID: "t",
	}); err != nil {
		t.Fatalf("StopCharging() error = %v", err)
	}
	completed, err := store.ConfirmStop(ctx, order.ConfirmStopCommand{
		OrderNo: created.OrderNo, ChargerID: chargerID, EnergyWh: 1000, OccurredAt: startedAt.Add(time.Minute),
	})
	if err != nil {
		t.Fatalf("ConfirmStop() error = %v", err)
	}
	return completed
}

// UC-U-09 presents payment confirmation and appeal as alternatives after the
// device stops. The old store rejected payment_status=PENDING, so the user saw
// a submit control but no row was ever written for the admin queue.
func TestUnsettledCompletedOrderAppealAppearsInAdminQueue(t *testing.T) {
	db, ctx := integrationDB(t)
	reviewStore, err := NewReviewStore(db)
	if err != nil {
		t.Fatalf("NewReviewStore() error = %v", err)
	}
	orderStore, err := NewOrderStore(db)
	if err != nil {
		t.Fatalf("NewOrderStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	userID, _, _, _, chargerID := orderFlowFixture(t, db, ctx, suffix)
	completed := completeUnsettledOrderForAppeal(t, ctx, orderStore, suffix, userID, chargerID)
	if completed.PaymentStatus != "PENDING" || completed.PaidCent != 0 {
		t.Fatalf("completed order payment = %s paid=%d, want PENDING/0", completed.PaymentStatus, completed.PaidCent)
	}

	created, err := reviewStore.CreateAppeal(ctx, userID, completed.OrderNo, "结束充电后的计量金额有误")
	if err != nil {
		t.Fatalf("CreateAppeal() error = %v", err)
	}
	if created.Status != review.AppealPending || created.OrderAmountCent <= 0 || created.OrderPaidCent != 0 {
		t.Fatalf("created appeal = %#v, want pending unpaid bill", created)
	}

	queue, err := reviewStore.ListAppeals(ctx, review.AppealFilter{Status: review.AppealPending, Page: 1, PageSize: 100})
	if err != nil {
		t.Fatalf("ListAppeals() error = %v", err)
	}
	found := false
	for _, item := range queue.Items {
		if item.ID == created.ID {
			found = item.OrderNo == completed.OrderNo && item.Reason == "结束充电后的计量金额有误"
			break
		}
	}
	if !found {
		t.Fatalf("admin pending queue does not contain appeal %#v", created)
	}
}

// The customer filed the appeal from the app and could then see nothing: the
// created response omitted the settled amount, so a client read "0 to refund"
// even on an order that had been paid in full.
func TestCreateAppealReportsTheSettledAmount(t *testing.T) {
	db, ctx := integrationDB(t)
	reviewStore, err := NewReviewStore(db)
	if err != nil {
		t.Fatalf("NewReviewStore() error = %v", err)
	}
	orderStore2, err := NewOrderStore(db)
	if err != nil {
		t.Fatalf("NewOrderStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	userA, _, _, _, chargerA := orderFlowFixture(t, db, ctx, suffix)
	created := completeOrderForReview(t, db, ctx, orderStore2, suffix, userA, chargerA)

	// completeOrderForReview settles the bill (and settlement is what makes the order
	// appealable at all: CreateAppeal refuses an unpaid completed order). Read the
	// settled amounts back from the order row — the create response carries the
	// amounts at creation time, when both were still zero.
	var amountCents, paidCents int64
	if err := db.QueryRowContext(ctx, `SELECT amount_cents, paid_cents FROM charging_orders WHERE order_no = $1`,
		created.OrderNo).Scan(&amountCents, &paidCents); err != nil {
		t.Fatalf("read settled amounts: %v", err)
	}
	if paidCents == 0 {
		t.Fatal("fixture did not settle the order; the assertion below would be vacuous")
	}

	appeal, err := reviewStore.CreateAppeal(ctx, userA, created.OrderNo, "计量有误，申请复核")
	if err != nil {
		t.Fatalf("CreateAppeal() error = %v", err)
	}
	if appeal.OrderPaidCent != paidCents {
		t.Fatalf("created appeal orderPaidCent = %d, want the settled %d", appeal.OrderPaidCent, paidCents)
	}
	if appeal.OrderAmountCent != amountCents {
		t.Fatalf("created appeal orderAmountCent = %d, want %d", appeal.OrderAmountCent, amountCents)
	}

	// The queue and the single-appeal read must agree with the create response.
	read, err := reviewStore.GetAppealByOrder(ctx, userA, created.OrderNo)
	if err != nil {
		t.Fatalf("GetAppealByOrder() error = %v", err)
	}
	if read.OrderPaidCent != paidCents || read.OrderAmountCent != amountCents {
		t.Fatalf("GetAppealByOrder() amounts = %d/%d, want %d/%d",
			read.OrderAmountCent, read.OrderPaidCent, amountCents, paidCents)
	}
}

// Rejection is the missing outcome: before it, the only way to close a bogus
// appeal was to approve it, which cancels the order and refunds the customer.
func TestRejectAppealDismissesWithoutRefunding(t *testing.T) {
	db, ctx := integrationDB(t)
	reviewStore, err := NewReviewStore(db)
	if err != nil {
		t.Fatalf("NewReviewStore() error = %v", err)
	}
	orderStore2, err := NewOrderStore(db)
	if err != nil {
		t.Fatalf("NewOrderStore() error = %v", err)
	}
	walletStore, err := NewWalletStore(db)
	if err != nil {
		t.Fatalf("NewWalletStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	userA, _, _, _, chargerA := orderFlowFixture(t, db, ctx, suffix)
	created := completeOrderForReview(t, db, ctx, orderStore2, suffix, userA, chargerA)
	appeal, err := reviewStore.CreateAppeal(ctx, userA, created.OrderNo, "计量有误，申请复核")
	if err != nil {
		t.Fatalf("CreateAppeal() error = %v", err)
	}
	before, err := walletStore.Wallet(ctx, userA)
	if err != nil {
		t.Fatalf("wallet before: %v", err)
	}

	adminID := appealAdminActor(t, db, ctx)
	rejected, err := reviewStore.RejectAppeal(ctx, appeal.ID, adminID, "计量与设备记录一致，申诉不成立")
	if err != nil || !rejected {
		t.Fatalf("RejectAppeal() = %v, %v", rejected, err)
	}

	// The order and the wallet are untouched: rejection refunds and cancels nothing.
	var orderStatus, paymentStatus string
	var paidCents int64
	if err := db.QueryRowContext(ctx, `SELECT status, payment_status, paid_cents FROM charging_orders WHERE order_no = $1`,
		created.OrderNo).Scan(&orderStatus, &paymentStatus, &paidCents); err != nil {
		t.Fatalf("order row: %v", err)
	}
	if orderStatus != "COMPLETED" || paymentStatus != "PAID" || paidCents == 0 {
		t.Fatalf("order after rejection = %s/%s paid=%d, want COMPLETED/PAID with the money still charged",
			orderStatus, paymentStatus, paidCents)
	}
	after, err := walletStore.Wallet(ctx, userA)
	if err != nil {
		t.Fatalf("wallet after: %v", err)
	}
	if after.BalanceCent != before.BalanceCent {
		t.Fatalf("balance after rejection = %d, want unchanged %d", after.BalanceCent, before.BalanceCent)
	}

	// The customer can read why, and the decision is audited.
	read, err := reviewStore.GetAppealByOrder(ctx, userA, created.OrderNo)
	if err != nil {
		t.Fatalf("GetAppealByOrder() error = %v", err)
	}
	if read.Status != review.AppealRejected {
		t.Fatalf("status = %s, want REJECTED", read.Status)
	}
	if read.DecisionReason == nil || *read.DecisionReason == "" {
		t.Fatalf("decisionReason = %v, want the operator's reason", read.DecisionReason)
	}
	if read.DecidedAt == nil {
		t.Fatal("decidedAt is missing after a decision")
	}
	var audited int64
	if err := db.QueryRowContext(ctx, `SELECT count(*) FROM operation_logs
WHERE action = 'appeal.reject' AND resource_id = $1`, fmt.Sprintf("%d", appeal.ID)).Scan(&audited); err != nil {
		t.Fatalf("audit count: %v", err)
	}
	if audited != 1 {
		t.Fatalf("appeal.reject audit rows = %d, want 1", audited)
	}

	// A decision is final: rejecting again, or approving afterwards, is a no-op.
	if rejected, err := reviewStore.RejectAppeal(ctx, appeal.ID, adminID, "再驳回一次"); err != nil || rejected {
		t.Fatalf("second reject = %v, %v; want false", rejected, err)
	}
	if approved, err := reviewStore.ApproveAppeal(ctx, appeal.ID, adminID); err != nil || approved {
		t.Fatalf("approve after reject = %v, %v; want false (a rejected appeal must not refund)", approved, err)
	}
	final, err := reviewStore.GetAppealByOrder(ctx, userA, created.OrderNo)
	if err != nil {
		t.Fatalf("GetAppealByOrder() after repeat = %v", err)
	}
	if final.Status != review.AppealRejected {
		t.Fatalf("status after repeat = %s, want REJECTED", final.Status)
	}
}

// A dismissed appeal must not keep blocking the order: 申诉禁止扣款和评论 is about
// live appeals, and treating REJECTED as live would lock the order out of reviews
// forever.
func TestRejectedAppealStopsBlockingTheOrder(t *testing.T) {
	db, ctx := integrationDB(t)
	reviewStore, err := NewReviewStore(db)
	if err != nil {
		t.Fatalf("NewReviewStore() error = %v", err)
	}
	orderStore2, err := NewOrderStore(db)
	if err != nil {
		t.Fatalf("NewOrderStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	userA, _, _, _, chargerA := orderFlowFixture(t, db, ctx, suffix)
	created := completeOrderForReview(t, db, ctx, orderStore2, suffix, userA, chargerA)

	appeal, err := reviewStore.CreateAppeal(ctx, userA, created.OrderNo, "计量有误，申请复核")
	if err != nil {
		t.Fatalf("CreateAppeal() error = %v", err)
	}
	// While the appeal is live the order cannot be reviewed (UC-U-09).
	if _, err := reviewStore.CreateReview(ctx, userA, created.OrderNo, 5, "live appeal blocks this"); !errorsIsReview(err, review.ErrOrderNotReviewable) {
		t.Fatalf("review under a live appeal = %v, want ErrOrderNotReviewable", err)
	}

	adminID := appealAdminActor(t, db, ctx)
	if rejected, err := reviewStore.RejectAppeal(ctx, appeal.ID, adminID, "申诉不成立"); err != nil || !rejected {
		t.Fatalf("RejectAppeal() = %v, %v", rejected, err)
	}
	if _, err := reviewStore.CreateReview(ctx, userA, created.OrderNo, 5, "驳回后可以评价了"); err != nil {
		t.Fatalf("review after rejection error = %v, want success", err)
	}
}

// One customer must never read another customer's appeal.
func TestGetAppealByOrderIsScopedToTheCaller(t *testing.T) {
	db, ctx := integrationDB(t)
	reviewStore, err := NewReviewStore(db)
	if err != nil {
		t.Fatalf("NewReviewStore() error = %v", err)
	}
	orderStore2, err := NewOrderStore(db)
	if err != nil {
		t.Fatalf("NewOrderStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	userA, userB, _, _, chargerA := orderFlowFixture(t, db, ctx, suffix)
	created := completeOrderForReview(t, db, ctx, orderStore2, suffix, userA, chargerA)
	if _, err := reviewStore.CreateAppeal(ctx, userA, created.OrderNo, "计量有误，申请复核"); err != nil {
		t.Fatalf("CreateAppeal() error = %v", err)
	}

	if _, err := reviewStore.GetAppealByOrder(ctx, userB, created.OrderNo); !errorsIsReview(err, review.ErrNotFound) {
		t.Fatalf("other user's read error = %v, want ErrNotFound", err)
	}
	if _, err := reviewStore.GetAppealByOrder(ctx, userA, "ORD-NOT-THERE"); !errorsIsReview(err, review.ErrNotFound) {
		t.Fatalf("missing order read error = %v, want ErrNotFound", err)
	}
}

// Every appeal read path must project the same columns: an extra column in the
// SQL with a missing scan destination fails at runtime with "expected N
// destination arguments", and only a real database read shows it (the handler
// tests use a fake store, so an unmatched projection slipped through once and
// made every approve/reject response a 503 while the decision itself was stored).
func TestAppealReadPathsProjectTheSameColumns(t *testing.T) {
	db, ctx := integrationDB(t)
	reviewStore, err := NewReviewStore(db)
	if err != nil {
		t.Fatalf("NewReviewStore() error = %v", err)
	}
	orderStore2, err := NewOrderStore(db)
	if err != nil {
		t.Fatalf("NewOrderStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	userA, _, _, _, chargerA := orderFlowFixture(t, db, ctx, suffix)
	created := completeOrderForReview(t, db, ctx, orderStore2, suffix, userA, chargerA)
	appeal, err := reviewStore.CreateAppeal(ctx, userA, created.OrderNo, "计量有误，申请复核")
	if err != nil {
		t.Fatalf("CreateAppeal() error = %v", err)
	}
	adminID := appealAdminActor(t, db, ctx)
	if rejected, err := reviewStore.RejectAppeal(ctx, appeal.ID, adminID, "申诉不成立"); err != nil || !rejected {
		t.Fatalf("RejectAppeal() = %v, %v", rejected, err)
	}

	// By id (the endpoint returns this one after a decision) ...
	byID, err := reviewStore.GetAppeal(ctx, appeal.ID)
	if err != nil {
		t.Fatalf("GetAppeal() error = %v", err)
	}
	// ... by order for the author ...
	byOrder, err := reviewStore.GetAppealByOrder(ctx, userA, created.OrderNo)
	if err != nil {
		t.Fatalf("GetAppealByOrder() error = %v", err)
	}
	// ... and through the queue.
	queue, err := reviewStore.ListAppeals(ctx, review.AppealFilter{Status: review.AppealRejected, Page: 1, PageSize: 100})
	if err != nil {
		t.Fatalf("ListAppeals() error = %v", err)
	}
	var queued *review.AppealView
	for index := range queue.Items {
		if queue.Items[index].ID == appeal.ID {
			queued = &queue.Items[index]
		}
	}
	if queued == nil {
		t.Fatalf("rejected appeal %d missing from the queue", appeal.ID)
	}

	for name, view := range map[string]review.AppealView{"GetAppeal": byID, "GetAppealByOrder": byOrder, "ListAppeals": *queued} {
		if view.Status != review.AppealRejected {
			t.Fatalf("%s status = %s, want REJECTED", name, view.Status)
		}
		if view.DecisionReason == nil || *view.DecisionReason != "申诉不成立" {
			t.Fatalf("%s decisionReason = %v, want the stored reason", name, view.DecisionReason)
		}
		if view.OrderPaidCent == 0 || view.OrderAmountCent == 0 {
			t.Fatalf("%s amounts = %d/%d, want the settled order amounts", name, view.OrderAmountCent, view.OrderPaidCent)
		}
	}
}
