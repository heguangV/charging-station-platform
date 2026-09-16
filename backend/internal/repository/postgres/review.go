package postgres

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"time"

	"github.com/heguangV/charging-station-platform/backend/internal/review"
)

// ReviewStore implements review.Store. Delivered by A-05 as the B-02
// handoff: the port lives in internal/review, the PostgreSQL adapter here.
// Mutations run in one transaction covering the review/appeal row, the
// order state and (for approved appeals) the wallet refund.
type ReviewStore struct {
	db *sql.DB
}

// NewReviewStore binds the store to a connection pool.
func NewReviewStore(db *sql.DB) (*ReviewStore, error) {
	if db == nil {
		return nil, errors.New("postgres: review store requires a database")
	}
	return &ReviewStore{db: db}, nil
}

// reviewability check shared by CreateReview and CreateAppeal: the order
// must be COMPLETED, owned by the user, and returns its id, station and
// payment state.
func (s *ReviewStore) checkOrderUsable(tx *sql.Tx, ctx context.Context, userID int64, orderNo string) (int64, int64, int64, int64, string, error) {
	var orderID, stationID, amountCents, paidCents int64
	var status, paymentStatus string
	err := tx.QueryRowContext(ctx, `SELECT id, station_id, amount_cents, paid_cents, status, payment_status FROM charging_orders
WHERE order_no = $1 AND user_id = $2 AND status = 'COMPLETED' FOR UPDATE`,
		orderNo, userID).Scan(&orderID, &stationID, &amountCents, &paidCents, &status, &paymentStatus)
	if errors.Is(err, sql.ErrNoRows) {
		// Cross-user access is indistinguishable from a missing record
		// (UC-U-12: 无权限访问按订单不存在处理).
		return 0, 0, 0, 0, "", review.ErrNotOrderOwner
	}
	if err != nil {
		return 0, 0, 0, 0, "", err
	}
	return orderID, stationID, amountCents, paidCents, paymentStatus, nil
}

// blockedByAppeal reports whether a live appeal blocks the requested mutation
// (UC-U-09: 申诉禁止扣款和评论). A REJECTED appeal is dismissed and therefore
// does not block anything - otherwise a dismissed appeal would lock the order
// out of reviews forever. The order's business number is the appeals table key.
func (s *ReviewStore) blockedByAppeal(tx *sql.Tx, ctx context.Context, orderNo string) (bool, error) {
	var blocked int64
	err := tx.QueryRowContext(ctx, `SELECT count(*) FROM order_appeals
WHERE order_no = $1 AND status <> 'REJECTED'`, orderNo).Scan(&blocked)
	if err != nil {
		return false, err
	}
	return blocked > 0, nil
}

// CreateReview inserts the review. Reviewability: the order is COMPLETED,
// owned by the user, and no appeal exists for it. The unique order index is
// the concurrency guard; identical-content replays return the stored row.
func (s *ReviewStore) CreateReview(ctx context.Context, userID int64, orderNo string, stars int, comment string) (review.ReviewView, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return review.ReviewView{}, err
	}
	defer func() { _ = tx.Rollback() }()

	if _, _, _, _, _, err := s.checkOrderUsable(tx, ctx, userID, orderNo); err != nil {
		return review.ReviewView{}, err
	}
	if blocked, err := s.blockedByAppeal(tx, ctx, orderNo); err != nil || blocked {
		return review.ReviewView{}, review.ErrOrderNotReviewable
	}

	var existingStars int
	var existingComment string
	var existingCreatedAt time.Time
	err = tx.QueryRowContext(ctx, `SELECT stars, comment, created_at FROM order_reviews WHERE order_no = $1`, orderNo).Scan(&existingStars, &existingComment, &existingCreatedAt)
	if err == nil {
		// Same content replays the first result; different content conflicts.
		// The replay carries the stored creation time, not a fabricated one —
		// the replayed view must be identical to the first response.
		if existingStars == stars && existingComment == comment {
			return review.ReviewView{OrderNo: orderNo, Stars: existingStars, Comment: existingComment, CreatedAt: existingCreatedAt.UTC()}, nil
		}
		return review.ReviewView{}, review.ErrReviewConflict
	}
	if !errors.Is(err, sql.ErrNoRows) {
		return review.ReviewView{}, err
	}

	var createdAt time.Time
	if err := tx.QueryRowContext(ctx, `INSERT INTO order_reviews (order_no, user_id, station_id, stars, comment)
VALUES ($1, $2, (SELECT station_id FROM charging_orders WHERE order_no = $1), $3, $4)
RETURNING created_at`, orderNo, userID, stars, comment).Scan(&createdAt); err != nil {
		return review.ReviewView{}, err
	}

	view := review.ReviewView{OrderNo: orderNo, Stars: stars, Comment: comment, CreatedAt: createdAt.UTC()}
	if err := tx.Commit(); err != nil {
		return review.ReviewView{}, err
	}
	return view, nil
}

// GetReview returns the user's own review. The review's association key is
// the order business number (order_reviews.order_no, migration 0009), which
// is UNIQUE on both sides of the join.
func (s *ReviewStore) GetReview(ctx context.Context, userID int64, orderNo string) (review.ReviewView, error) {
	const query = `SELECT r.order_no, r.stars, r.comment, r.created_at FROM order_reviews r
JOIN charging_orders o ON o.order_no = r.order_no
WHERE r.order_no = $1 AND o.user_id = $2`
	var view review.ReviewView
	var createdAt time.Time
	err := s.db.QueryRowContext(ctx, query, orderNo, userID).Scan(&view.OrderNo, &view.Stars, &view.Comment, &createdAt)
	if errors.Is(err, sql.ErrNoRows) {
		return review.ReviewView{}, review.ErrNotFound
	}
	if err != nil {
		return review.ReviewView{}, err
	}
	view.CreatedAt = createdAt.UTC()
	return view, nil
}

// ListWall returns one page of the station comment wall, newest first, with
// authors masked: nickname, masked phone or 已注销用户.
func (s *ReviewStore) ListWall(ctx context.Context, filter review.WallFilter) (review.WallPage, error) {
	const pageQuery = `SELECT r.stars, r.comment, r.created_at,
CASE
  WHEN u.deleted_at IS NOT NULL THEN '已注销用户'
  WHEN u.display_name <> '' THEN u.display_name
  WHEN u.phone IS NOT NULL AND length(u.phone) = 11 THEN substr(u.phone, 1, 3) || '****' || substr(u.phone, 8)
  ELSE '***'
END AS author
FROM order_reviews r
JOIN user_accounts u ON u.id = r.user_id
WHERE r.station_id = $1
ORDER BY r.created_at DESC
LIMIT $2 OFFSET $3`
	const countQuery = `SELECT count(*) FROM order_reviews WHERE station_id = $1`

	offset := (filter.Page - 1) * filter.PageSize
	rows, err := s.db.QueryContext(ctx, pageQuery, filter.StationID, filter.PageSize, offset)
	if err != nil {
		return review.WallPage{}, err
	}
	defer rows.Close()

	page := review.WallPage{Meta: review.PageMeta{Page: filter.Page, PageSize: filter.PageSize}}
	for rows.Next() {
		var entry review.WallEntry
		if err := rows.Scan(&entry.Stars, &entry.Comment, &entry.CreatedAt, &entry.Author); err != nil {
			return review.WallPage{}, err
		}
		entry.CreatedAt = entry.CreatedAt.UTC()
		page.Items = append(page.Items, entry)
	}
	if err := rows.Err(); err != nil {
		return review.WallPage{}, err
	}
	if err := s.db.QueryRowContext(ctx, countQuery, filter.StationID).Scan(&page.Meta.Total); err != nil {
		return review.WallPage{}, err
	}
	return page, nil
}

// CreateAppeal records the appeal. Reviewability mirrors CreateReview: the
// COMPLETED owner order. At most one appeal per order (UC-U-09): the unique
// order index is the concurrency guard and, because the replay lookup runs
// after the order lock, a same-content retry — including two racing submits
// — returns the stored appeal, while a different-content one conflicts. An
// A completed bill is appealable before payment confirmation: UC-U-09 makes
// "confirm payment" and "appeal" the two alternatives presented after a
// charge stops. A live appeal then blocks settlement until an administrator
// decides it.
func (s *ReviewStore) CreateAppeal(ctx context.Context, userID int64, orderNo string, reason string) (review.AppealView, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return review.AppealView{}, err
	}
	defer func() { _ = tx.Rollback() }()

	_, _, amountCents, paidCents, _, err := s.checkOrderUsable(tx, ctx, userID, orderNo)
	if err != nil {
		return review.AppealView{}, err
	}

	// At most one appeal per order: a same-content replay returns the first
	// result, a different-content one conflicts. The read runs after the
	// order lock, so an appeal another transaction just committed is visible
	// here and replays instead of hitting the unique index.
	var existing review.AppealView
	var decidedAt sql.NullTime
	err = tx.QueryRowContext(ctx, `SELECT a.id, a.reason, a.status, a.created_at, a.decided_at,
COALESCE(o.amount_cents, 0), COALESCE(o.paid_cents, 0), a.decision_reason
FROM order_appeals a
LEFT JOIN charging_orders o ON o.order_no = a.order_no
WHERE a.order_no = $1`, orderNo).Scan(&existing.ID, &existing.Reason, &existing.Status,
		&existing.CreatedAt, &decidedAt, &existing.OrderAmountCent, &existing.OrderPaidCent, &existing.DecisionReason)
	if err == nil {
		if existing.Reason != reason {
			return review.AppealView{}, review.ErrAppealConflict
		}
		existing.OrderNo = orderNo
		existing.CreatedAt = existing.CreatedAt.UTC()
		if decidedAt.Valid {
			value := decidedAt.Time.UTC()
			existing.DecidedAt = &value
		}
		return existing, nil
	}
	if !errors.Is(err, sql.ErrNoRows) {
		return review.AppealView{}, err
	}

	var appealID int64
	var createdAt time.Time
	err = tx.QueryRowContext(ctx, `INSERT INTO order_appeals (order_no, user_id, reason, status)
VALUES ($1, $2, $3, 'PENDING')
RETURNING id, created_at`, orderNo, userID, reason).Scan(&appealID, &createdAt)
	if err != nil {
		return review.AppealView{}, err
	}

	view := review.AppealView{
		ID:      appealID,
		OrderNo: orderNo, Reason: reason, Status: review.AppealPending,
		OrderAmountCent: amountCents,
		// The settled amount is what an approval would refund, so it has to be
		// part of the response: leaving it out reported "0 to refund" on an order
		// that was paid in full.
		OrderPaidCent: paidCents,
		CreatedAt:     createdAt.UTC(),
	}
	if err := tx.Commit(); err != nil {
		return review.AppealView{}, err
	}
	return view, nil
}

// ListAppeals returns one page of the admin appeal queue.
func (s *ReviewStore) ListAppeals(ctx context.Context, filter review.AppealFilter) (review.AppealPage, error) {
	const filterSQL = `($1 = '' OR a.status = $1)`
	const pageQuery = `SELECT a.id, a.order_no, a.reason, a.status, a.created_at, a.decided_at,
COALESCE(o.amount_cents, 0), COALESCE(o.paid_cents, 0), a.decision_reason
FROM order_appeals a
LEFT JOIN charging_orders o ON o.order_no = a.order_no
WHERE ` + filterSQL + `
ORDER BY a.created_at DESC
LIMIT $2 OFFSET $3`
	const countQuery = `SELECT count(*) FROM order_appeals a WHERE ` + filterSQL

	offset := (filter.Page - 1) * filter.PageSize
	rows, err := s.db.QueryContext(ctx, pageQuery, filter.Status, filter.PageSize, offset)
	if err != nil {
		return review.AppealPage{}, err
	}
	defer rows.Close()

	page := review.AppealPage{Meta: review.PageMeta{Page: filter.Page, PageSize: filter.PageSize}}
	for rows.Next() {
		var view review.AppealView
		var decidedAt sql.NullTime
		if err := rows.Scan(&view.ID, &view.OrderNo, &view.Reason, &view.Status, &view.CreatedAt,
			&decidedAt, &view.OrderAmountCent, &view.OrderPaidCent, &view.DecisionReason); err != nil {
			return review.AppealPage{}, err
		}
		if decidedAt.Valid {
			value := decidedAt.Time.UTC()
			view.DecidedAt = &value
		}
		view.CreatedAt = view.CreatedAt.UTC()
		page.Items = append(page.Items, view)
	}
	if err := rows.Err(); err != nil {
		return review.AppealPage{}, err
	}
	if err := s.db.QueryRowContext(ctx, countQuery, filter.Status).Scan(&page.Meta.Total); err != nil {
		return review.AppealPage{}, err
	}
	return page, nil
}

// GetAppeal returns one appeal by id.
func (s *ReviewStore) GetAppeal(ctx context.Context, appealID int64) (review.AppealView, error) {
	const query = `SELECT a.id, a.order_no, a.reason, a.status, a.created_at, a.decided_at,
COALESCE(o.amount_cents, 0), COALESCE(o.paid_cents, 0), a.decision_reason
FROM order_appeals a
LEFT JOIN charging_orders o ON o.order_no = a.order_no
WHERE a.id = $1`
	var view review.AppealView
	var decidedAt sql.NullTime
	err := s.db.QueryRowContext(ctx, query, appealID).Scan(&view.ID, &view.OrderNo, &view.Reason,
		&view.Status, &view.CreatedAt, &decidedAt, &view.OrderAmountCent, &view.OrderPaidCent,
		&view.DecisionReason)
	if errors.Is(err, sql.ErrNoRows) {
		return review.AppealView{}, review.ErrNotFound
	}
	if err != nil {
		return review.AppealView{}, err
	}
	if decidedAt.Valid {
		value := decidedAt.Time.UTC()
		view.DecidedAt = &value
	}
	view.CreatedAt = view.CreatedAt.UTC()
	return view, nil
}

// GetAppealByOrder returns the caller's own appeal for one order.
//
// Scoped by user_id so one customer can never read another's appeal; a missing
// appeal and someone else's appeal both come back as ErrNotFound.
func (s *ReviewStore) GetAppealByOrder(ctx context.Context, userID int64, orderNo string) (review.AppealView, error) {
	const query = `SELECT a.id, a.order_no, a.reason, a.status, a.created_at, a.decided_at,
COALESCE(o.amount_cents, 0), COALESCE(o.paid_cents, 0), a.decision_reason
FROM order_appeals a
LEFT JOIN charging_orders o ON o.order_no = a.order_no
WHERE a.order_no = $1 AND a.user_id = $2`
	var view review.AppealView
	var decidedAt sql.NullTime
	err := s.db.QueryRowContext(ctx, query, orderNo, userID).Scan(&view.ID, &view.OrderNo, &view.Reason,
		&view.Status, &view.CreatedAt, &decidedAt, &view.OrderAmountCent, &view.OrderPaidCent,
		&view.DecisionReason)
	if errors.Is(err, sql.ErrNoRows) {
		return review.AppealView{}, review.ErrNotFound
	}
	if err != nil {
		return review.AppealView{}, err
	}
	if decidedAt.Valid {
		value := decidedAt.Time.UTC()
		view.DecidedAt = &value
	}
	view.CreatedAt = view.CreatedAt.UTC()
	return view, nil
}

// RejectAppeal records the admin decision to dismiss an appeal in one
// transaction: the appeal becomes REJECTED with the operator's reason, and the
// order and the wallet stay exactly as they are (rejection refunds nothing and
// cancels nothing - that is what approval is for). Returns false when the
// appeal already carries a decision, which the caller treats as a no-op.
func (s *ReviewStore) RejectAppeal(ctx context.Context, appealID int64, adminID int64, reason string) (bool, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return false, err
	}
	defer func() { _ = tx.Rollback() }()

	var orderNo, appealStatus string
	err = tx.QueryRowContext(ctx, `SELECT order_no, status FROM order_appeals WHERE id = $1 FOR UPDATE`,
		appealID).Scan(&orderNo, &appealStatus)
	if errors.Is(err, sql.ErrNoRows) {
		return false, review.ErrNotFound
	}
	if err != nil {
		return false, err
	}
	// A decision is final: approving after rejecting (or rejecting twice) never
	// re-acts, so a repeated request cannot flip a refunded order.
	if appealStatus != review.AppealPending {
		return false, nil
	}

	if _, err := tx.ExecContext(ctx, `UPDATE order_appeals
SET status = 'REJECTED', decided_by = $2, decided_at = CURRENT_TIMESTAMP, decision_reason = $3
WHERE id = $1`, appealID, adminID, reason); err != nil {
		return false, err
	}
	if _, err := tx.ExecContext(ctx, `INSERT INTO operation_logs
    (actor_type, actor_id, action, resource_type, resource_id, request_id, payload)
VALUES ('ADMIN', $1, 'appeal.reject', 'appeal', $2, '', $3::jsonb)`,
		fmt.Sprintf("%d", adminID), fmt.Sprintf("%d", appealID),
		fmt.Sprintf(`{"orderNo":%q,"reason":%q}`, orderNo, reason)); err != nil {
		return false, err
	}

	if err := tx.Commit(); err != nil {
		return false, err
	}
	return true, nil
}

// walletPaymentPending mirrors the order payment state without importing
// the order package (avoids a cross-domain dependency cycle).
const walletPaymentPending = "PENDING"

// ApproveAppeal applies the admin decision in one transaction: the appeal
// is marked APPROVED, the order is cancelled, the charger released and the
// settled amount refunded to the user's wallet. Returns false when the
// appeal was already approved (duplicate decisions are no-ops).
func (s *ReviewStore) ApproveAppeal(ctx context.Context, appealID int64, adminID int64) (bool, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return false, err
	}
	defer func() { _ = tx.Rollback() }()

	var orderNo, appealStatus string
	var orderID, paidCents int64
	err = tx.QueryRowContext(ctx, `SELECT a.order_no, a.status, o.id, COALESCE(o.paid_cents, 0)
FROM order_appeals a
LEFT JOIN charging_orders o ON o.order_no = a.order_no
WHERE a.id = $1 FOR UPDATE OF a`, appealID).Scan(&orderNo, &appealStatus, &orderID, &paidCents)
	if errors.Is(err, sql.ErrNoRows) {
		return false, review.ErrNotFound
	}
	if err != nil {
		return false, err
	}
	if appealStatus != review.AppealPending {
		return false, nil
	}

	var walletUser int64
	err = tx.QueryRowContext(ctx, `SELECT user_id FROM charging_orders WHERE id = $1 FOR UPDATE`, orderID).Scan(&walletUser)
	if err != nil {
		return false, err
	}

	// Refund what was actually deducted (appeal approvals never deduct).
	if paidCents > 0 {
		// B-07's walletBalanceForUpdate pattern, inlined for the review
		// store: the upsert creates the row when missing and takes its lock
		// in the same statement that returns the balance, so a concurrent
		// top-up can neither slip between the read and the refund nor leave
		// the ledger's balance_before pointing at a balance that never
		// existed.
		var userBalance int64
		if err := tx.QueryRowContext(ctx, `INSERT INTO wallet_accounts (user_id, balance_cents)
VALUES ($1, 0)
ON CONFLICT (user_id) DO UPDATE SET user_id = EXCLUDED.user_id
RETURNING balance_cents`, walletUser).Scan(&userBalance); err != nil {
			return false, err
		}
		if _, err := tx.ExecContext(ctx, `UPDATE wallet_accounts
SET balance_cents = balance_cents + $2, version = version + 1, updated_at = CURRENT_TIMESTAMP
WHERE user_id = $1`, walletUser, paidCents); err != nil {
			return false, err
		}
		if _, err := tx.ExecContext(ctx, `INSERT INTO wallet_transactions
    (user_id, order_id, transaction_type, amount_cents, balance_before_cents, balance_after_cents, idempotency_key)
VALUES ($1, $2, 'REFUND', $3, $4, $5, $6)`,
			walletUser, orderID, paidCents, userBalance, userBalance+paidCents,
			fmt.Sprintf("appeal:%d", appealID)); err != nil {
			return false, err
		}
	}

	if orderID > 0 {
		if _, err := tx.ExecContext(ctx, `UPDATE charging_orders
SET status = 'CANCELLED', version = version + 1, updated_at = CURRENT_TIMESTAMP
WHERE id = $1`, orderID); err != nil {
			return false, err
		}
		if _, err := tx.ExecContext(ctx, `UPDATE chargers SET status = 'IDLE', updated_at = CURRENT_TIMESTAMP
WHERE id = (SELECT charger_id FROM charging_orders WHERE id = $1) AND status = 'OCCUPIED'`, orderID); err != nil {
			return false, err
		}
	}

	if _, err := tx.ExecContext(ctx, `UPDATE order_appeals
SET status = 'APPROVED', decided_by = $2, decided_at = CURRENT_TIMESTAMP
WHERE id = $1`, appealID, adminID); err != nil {
		return false, err
	}
	if _, err := tx.ExecContext(ctx, `INSERT INTO operation_logs
    (actor_type, actor_id, action, resource_type, resource_id, request_id, payload)
VALUES ('ADMIN', $1, 'appeal.approve', 'appeal', $2, '', $3::jsonb)`,
		fmt.Sprintf("%d", adminID), fmt.Sprintf("%d", appealID),
		fmt.Sprintf(`{"orderNo":%q,"refundCent":%d}`, orderNo, paidCents)); err != nil {
		return false, err
	}

	if err := tx.Commit(); err != nil {
		return false, err
	}
	return true, nil
}
