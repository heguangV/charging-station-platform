package postgres

import (
	"context"
	"database/sql"
	"errors"
	"strconv"
	"testing"
	"time"

	"github.com/heguangV/charging-station-platform/backend/internal/order"
	"github.com/heguangV/charging-station-platform/backend/internal/review"
	"github.com/heguangV/charging-station-platform/backend/internal/wallet"
)

// completeOrderForReview drives an order through the full lifecycle to
// COMPLETED and settled (UC-U-12 precondition).
func completeOrderForReview(t *testing.T, db *sql.DB, ctx context.Context, store *OrderStore, suffix string, userID, chargerA int64) order.Order {
	t.Helper()
	created, err := store.CreateOrder(ctx, order.CreateOrderCommand{
		UserID: userID, ChargerID: chargerA, IdempotencyKey: "rv-create-" + suffix, RequestHash: "h", TraceID: "t",
	})
	if err != nil {
		t.Fatalf("CreateOrder() error = %v", err)
	}
	if _, err := store.StartCharging(ctx, order.TransitionCommand{
		UserID: userID, OrderNo: created.OrderNo, IdempotencyKey: "rv-start-" + suffix, RequestHash: "h", TraceID: "t"}); err != nil {
		t.Fatalf("StartCharging() error = %v", err)
	}
	if _, err := store.ConfirmStart(ctx, order.ConfirmStartCommand{
		OrderNo: created.OrderNo, ChargerID: chargerA, OccurredAt: time.Now().UTC()}); err != nil {
		t.Fatalf("ConfirmStart() error = %v", err)
	}
	if _, err := store.StopCharging(ctx, order.TransitionCommand{
		UserID: userID, OrderNo: created.OrderNo, IdempotencyKey: "rv-stop-" + suffix, RequestHash: "h", TraceID: "t"}); err != nil {
		t.Fatalf("StopCharging() error = %v", err)
	}
	if _, err := store.ConfirmStop(ctx, order.ConfirmStopCommand{
		OrderNo: created.OrderNo, ChargerID: chargerA, EnergyWh: 1000, OccurredAt: time.Now().UTC()}); err != nil {
		t.Fatalf("ConfirmStop() error = %v", err)
	}
	if _, err := store.SettleOrder(ctx, order.SettleCommand{
		TransitionCommand: order.TransitionCommand{
			UserID: userID, OrderNo: created.OrderNo, IdempotencyKey: "rv-settle-" + suffix, RequestHash: "h", TraceID: "t"}}); err != nil {
		t.Fatalf("SettleOrder() error = %v", err)
	}
	return created
}

func errorsIsReview(err error, target error) bool {
	return errors.Is(err, target)
}

// integrationDB applies the embedded migration set — 0009 included, through
// the same runner the API uses at startup — so every review test below runs
// against the real production schema.

func TestReviewLifecycleAndWall(t *testing.T) {
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
	userA, userB, stationA, _, chargerA := orderFlowFixture(t, db, ctx, suffix)

	// Give the review author a display name so the wall shows it.
	if _, err := db.ExecContext(ctx, `UPDATE user_accounts SET display_name = '用户0606' WHERE id = $1`, userA); err != nil {
		t.Fatalf("set display name: %v", err)
	}

	created := completeOrderForReview(t, db, ctx, orderStore2, suffix, userA, chargerA)

	// Cross-user review is indistinguishable from a missing order.
	if _, err := reviewStore.CreateReview(ctx, userB, created.OrderNo, 5, "很好"); !errorsIsReview(err, review.ErrNotOrderOwner) {
		t.Fatalf("cross-user review error = %v, want ErrNotOrderOwner", err)
	}

	// Identical replay returns the first result; different content conflicts.
	first, err := reviewStore.CreateReview(ctx, userA, created.OrderNo, 5, "充电很快")
	if err != nil {
		t.Fatalf("CreateReview() error = %v", err)
	}
	replay, err := reviewStore.CreateReview(ctx, userA, created.OrderNo, 5, "充电很快")
	if err != nil || replay.Comment != first.Comment {
		t.Fatalf("replay = %#v, %v", replay, err)
	}
	// The first response, the replay and the later read must all carry the
	// stored creation time — Review.createdAt is required by the contract.
	if first.CreatedAt.IsZero() {
		t.Fatalf("first review createdAt = %v, want the stored time", first.CreatedAt)
	}
	if !replay.CreatedAt.Equal(first.CreatedAt) {
		t.Fatalf("replay createdAt = %v, want the first result's %v", replay.CreatedAt, first.CreatedAt)
	}
	if _, err := reviewStore.CreateReview(ctx, userA, created.OrderNo, 3, "一般"); !errorsIsReview(err, review.ErrReviewConflict) {
		t.Fatalf("different content error = %v, want ErrReviewConflict", err)
	}

	// GET /orders/{orderNo}/review: the author reads the stored review,
	// everyone else — including another logged-in user and a unknown order —
	// gets the same "not found" answer.
	got, err := reviewStore.GetReview(ctx, userA, created.OrderNo)
	if err != nil || got.OrderNo != created.OrderNo || got.Stars != 5 || got.Comment != "充电很快" {
		t.Fatalf("GetReview() = %#v, %v", got, err)
	}
	if !got.CreatedAt.Equal(first.CreatedAt) {
		t.Fatalf("GetReview createdAt = %v, want the stored %v", got.CreatedAt, first.CreatedAt)
	}
	if _, err := reviewStore.GetReview(ctx, userB, created.OrderNo); !errorsIsReview(err, review.ErrNotFound) {
		t.Fatalf("cross-user GetReview error = %v, want ErrNotFound", err)
	}
	if _, err := reviewStore.GetReview(ctx, userA, "ORD-NO-SUCH-"+suffix); !errorsIsReview(err, review.ErrNotFound) {
		t.Fatalf("unknown order GetReview error = %v, want ErrNotFound", err)
	}

	// The wall shows the review with the masked author.
	// The shared database accumulates rows from earlier runs; assert on the
	// newest entry (the wall is ordered newest first) rather than counts.
	wall, err := reviewStore.ListWall(ctx, review.WallFilter{StationID: stationA, Page: 1, PageSize: 20})
	if err != nil || len(wall.Items) == 0 {
		t.Fatalf("wall = %#v, %v", wall, err)
	}
	if wall.Items[0].Author != "用户0606" || wall.Items[0].Comment != "充电很快" {
		t.Fatalf("newest wall entry = %#v", wall.Items[0])
	}
	var missingStationID int64
	if err := db.QueryRowContext(ctx, `SELECT COALESCE(MAX(id), 0) + 1 FROM stations`).Scan(&missingStationID); err != nil {
		t.Fatalf("find missing station id: %v", err)
	}
	if _, err := reviewStore.ListWall(ctx, review.WallFilter{StationID: missingStationID, Page: 1, PageSize: 20}); !errorsIsReview(err, review.ErrNotFound) {
		t.Fatalf("missing station wall error = %v, want ErrNotFound", err)
	}

	// The first appeal succeeds (review and appeal are independent); a
	// same-content retry replays the first result and a different content
	// conflicts.
	firstAppeal, err := reviewStore.CreateAppeal(ctx, userA, created.OrderNo, "计量有误")
	if err != nil {
		t.Fatalf("first appeal error = %v", err)
	}
	replayedAppeal, err := reviewStore.CreateAppeal(ctx, userA, created.OrderNo, "计量有误")
	if err != nil || replayedAppeal.ID != firstAppeal.ID || replayedAppeal.Status != review.AppealPending ||
		replayedAppeal.Reason != firstAppeal.Reason || replayedAppeal.OrderAmountCent != firstAppeal.OrderAmountCent {
		t.Fatalf("same-content appeal replay = %#v, %v; want the first result", replayedAppeal, err)
	}
	if _, err := reviewStore.CreateAppeal(ctx, userA, created.OrderNo, "不同原因"); !errorsIsReview(err, review.ErrAppealConflict) {
		t.Fatalf("second appeal error = %v, want ErrAppealConflict", err)
	}
}

func TestAppealQueueOrdersOldestFirstWithOrderNumberTieBreak(t *testing.T) {
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

	firstOrder := completeOrderForReview(t, db, ctx, orderStore, suffix+"-sort-a", userID, chargerID)
	first, err := reviewStore.CreateAppeal(ctx, userID, firstOrder.OrderNo, "排序测试一")
	if err != nil {
		t.Fatalf("first CreateAppeal() error = %v", err)
	}
	secondOrder := completeOrderForReview(t, db, ctx, orderStore, suffix+"-sort-b", userID, chargerID)
	second, err := reviewStore.CreateAppeal(ctx, userID, secondOrder.OrderNo, "排序测试二")
	if err != nil {
		t.Fatalf("second CreateAppeal() error = %v", err)
	}

	sharedTime := time.Date(2099, 1, 1, 0, 0, 0, 0, time.UTC)
	if _, err := db.ExecContext(ctx, `UPDATE order_appeals SET created_at = $1 WHERE id IN ($2, $3)`, sharedTime, first.ID, second.ID); err != nil {
		t.Fatalf("set shared appeal time: %v", err)
	}

	firstPage, err := reviewStore.ListAppeals(ctx, review.AppealFilter{Status: review.AppealPending, Page: 1, PageSize: 100})
	if err != nil {
		t.Fatalf("ListAppeals() first page error = %v", err)
	}
	lastPageNumber := (firstPage.Meta.Total + 99) / 100
	lastPage, err := reviewStore.ListAppeals(ctx, review.AppealFilter{Status: review.AppealPending, Page: lastPageNumber, PageSize: 100})
	if err != nil {
		t.Fatalf("ListAppeals() last page error = %v", err)
	}

	wantFirst, wantSecond := firstOrder.OrderNo, secondOrder.OrderNo
	if wantFirst > wantSecond {
		wantFirst, wantSecond = wantSecond, wantFirst
	}
	for index := 0; index+1 < len(lastPage.Items); index++ {
		if lastPage.Items[index].OrderNo == wantFirst && lastPage.Items[index+1].OrderNo == wantSecond {
			return
		}
	}
	t.Fatalf("same-time appeals are not ordered by order number: want %s before %s in %#v", wantFirst, wantSecond, lastPage.Items)
}

func TestAppealApprovalRefundsWallet(t *testing.T) {
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
	settled, err := walletStore.Wallet(ctx, userA)
	if err != nil {
		t.Fatalf("wallet: %v", err)
	}

	appeal, err := reviewStore.CreateAppeal(ctx, userA, created.OrderNo, "计量有误，申请复核")
	if err != nil {
		t.Fatalf("CreateAppeal() error = %v", err)
	}

	// Admin approval: appeal APPROVED, order CANCELLED, wallet refunded.
	if approved, err := reviewStore.ApproveAppeal(ctx, appeal.ID, 9); err != nil || !approved {
		t.Fatalf("ApproveAppeal() = %v, %v", approved, err)
	}

	var orderStatus string
	if err := db.QueryRowContext(ctx, `SELECT status FROM charging_orders WHERE order_no = $1`, created.OrderNo).Scan(&orderStatus); err != nil {
		t.Fatalf("order status: %v", err)
	}
	if orderStatus != "CANCELLED" {
		t.Fatalf("order status = %s, want CANCELLED", orderStatus)
	}
	after, err := walletStore.Wallet(ctx, userA)
	if err != nil {
		t.Fatalf("wallet after refund: %v", err)
	}
	if after.BalanceCent != settled.BalanceCent+120 {
		t.Fatalf("balance = %d, want %d (+120 refund)", after.BalanceCent, settled.BalanceCent)
	}

	// Duplicate approval is a no-op.
	if approved, err := reviewStore.ApproveAppeal(ctx, appeal.ID, 9); err != nil || approved {
		t.Fatalf("duplicate approve = %v, %v; want false", approved, err)
	}

	// The queue reports the approved appeal (shared DB: assert by ID).
	queue, err := reviewStore.ListAppeals(ctx, review.AppealFilter{Status: review.AppealApproved, Page: 1, PageSize: 100})
	if err != nil {
		t.Fatalf("queue error = %v", err)
	}
	found := false
	for _, item := range queue.Items {
		if item.ID == appeal.ID && item.Status == review.AppealApproved {
			found = true
		}
	}
	if !found {
		t.Fatalf("approved appeal %d not in queue: %#v", appeal.ID, queue.Items)
	}
}

// TestAppealApprovalRefundIsSerializedWithTopUp pins the wallet-side
// invariant of an approved appeal: the refund creates the wallet row when
// missing and reads the balance under that row's lock (B-07 pattern), so a
// top-up racing on a user without a wallet row cannot leave the ledger's
// balance_before pointing at a balance that never existed.
func TestAppealApprovalRefundIsSerializedWithTopUp(t *testing.T) {
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

	// Drop the wallet row so the race below starts from the dangerous state:
	// no row exists, and whichever transaction creates it must not silently
	// read a zero balance the other transaction has already moved.
	if _, err := db.ExecContext(ctx, `DELETE FROM wallet_accounts WHERE user_id = $1`, userA); err != nil {
		t.Fatalf("drop wallet row: %v", err)
	}

	topupKey := "rv-race-" + suffix
	barrier := make(chan struct{})
	errs := make(chan error, 2)
	go func() {
		<-barrier
		_, err := walletStore.TopUp(ctx, wallet.TopUpCommand{
			UserID: userA, AmountCent: 5000, IdempotencyKey: topupKey, RequestHash: "h", TraceID: "t",
		})
		errs <- err
	}()
	go func() {
		<-barrier
		_, err := reviewStore.ApproveAppeal(ctx, appeal.ID, 9)
		errs <- err
	}()
	close(barrier)
	for received := 0; received < 2; received++ {
		if err := <-errs; err != nil {
			t.Fatalf("raced operation error = %v", err)
		}
	}

	// The refund leg and the top-up leg must form one contiguous ledger
	// chain that starts at the fresh row's zero and ends at the final
	// balance, in either finish order.
	var finalBalance int64
	if err := db.QueryRowContext(ctx, `SELECT balance_cents FROM wallet_accounts WHERE user_id = $1`, userA).Scan(&finalBalance); err != nil {
		t.Fatalf("final balance: %v", err)
	}
	if finalBalance != 5120 {
		t.Fatalf("final balance = %d, want 5120 (120 refund + 5000 top-up)", finalBalance)
	}
	ledgerRows, err := db.QueryContext(ctx, `SELECT transaction_type, amount_cents, balance_before_cents, balance_after_cents
FROM wallet_transactions
WHERE user_id = $1 AND idempotency_key IN ($2, $3)`,
		userA, "topup:"+strconv.FormatInt(userA, 10)+":"+topupKey, "appeal:"+strconv.FormatInt(appeal.ID, 10))
	if err != nil {
		t.Fatalf("ledger rows: %v", err)
	}
	defer ledgerRows.Close()
	type ledgerLeg struct {
		kind           string
		amount, before int64
		after          int64
	}
	var legs []ledgerLeg
	for ledgerRows.Next() {
		var leg ledgerLeg
		if err := ledgerRows.Scan(&leg.kind, &leg.amount, &leg.before, &leg.after); err != nil {
			t.Fatalf("scan ledger leg: %v", err)
		}
		legs = append(legs, leg)
	}
	if err := ledgerRows.Err(); err != nil {
		t.Fatalf("read ledger legs: %v", err)
	}
	if len(legs) != 2 {
		t.Fatalf("ledger legs = %d, want exactly the refund and the top-up", len(legs))
	}
	if legs[0].after > legs[1].after {
		legs[0], legs[1] = legs[1], legs[0]
	}
	if legs[0].before != 0 {
		t.Fatalf("first leg (%s) balance_before = %d, want 0 for the fresh wallet row", legs[0].kind, legs[0].before)
	}
	if legs[1].before != legs[0].after {
		t.Fatalf("ledger chain broken: %s ends at %d but %s starts at %d", legs[0].kind, legs[0].after, legs[1].kind, legs[1].before)
	}
	if legs[1].after != finalBalance {
		t.Fatalf("ledger ends at %d but the wallet holds %d", legs[1].after, finalBalance)
	}
}
