package postgres

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"time"

	"github.com/heguangV/charging-station-platform/backend/internal/wallet"
)

// WalletStore implements wallet.Store. Delivered by A-04 as the B-02
// handoff: the port lives in internal/wallet, the PostgreSQL adapter here.
// Every mutation runs in one transaction covering the wallet row, the
// ledger row and (for refunds) the order payment state.
type WalletStore struct {
	db    *sql.DB
	clock func() time.Time
}

// NewWalletStore binds the store to a connection pool.
func NewWalletStore(db *sql.DB) (*WalletStore, error) {
	if db == nil {
		return nil, errors.New("postgres: wallet store requires a database")
	}
	return &WalletStore{db: db, clock: time.Now}, nil
}

// claimWalletIdempotency mirrors the order store's claim semantics (fresh
// slot, hash-checked replay, expired-record supersede) with a wallet scope.
func (s *WalletStore) claimWalletIdempotency(tx *sql.Tx, ctx context.Context, scope, key, requestHash string) ([]byte, bool, error) {
	tag, err := tx.ExecContext(ctx, `INSERT INTO idempotency_records (scope, idempotency_key, request_hash, status, expires_at)
VALUES ($1, $2, $3, 'IN_PROGRESS', $4)
ON CONFLICT (scope, idempotency_key) DO NOTHING`,
		scope, key, requestHash, s.clock().Add(idempotencyTTL))
	if err != nil {
		return nil, false, err
	}
	if affected, err := tag.RowsAffected(); err == nil && affected == 1 {
		return nil, false, nil
	}

	var storedHash, status string
	var body []byte
	var expiresAt time.Time
	err = tx.QueryRowContext(ctx, `SELECT request_hash, status, response_body, expires_at
FROM idempotency_records WHERE scope = $1 AND idempotency_key = $2 FOR UPDATE`, scope, key).Scan(&storedHash, &status, &body, &expiresAt)
	if errors.Is(err, sql.ErrNoRows) {
		// The row vanished between the conflict and the select: another transaction is still writing it,
		// so this request must come back rather than be told the order cannot be refunded.
		return nil, false, wallet.ErrIdempotencyInProgress
	}
	if err != nil {
		return nil, false, err
	}
	if s.clock().After(expiresAt) {
		if _, err := tx.ExecContext(ctx, `DELETE FROM idempotency_records WHERE scope = $1 AND idempotency_key = $2`, scope, key); err != nil {
			return nil, false, err
		}
		if _, err := tx.ExecContext(ctx, `INSERT INTO idempotency_records (scope, idempotency_key, request_hash, status, expires_at)
VALUES ($1, $2, $3, 'IN_PROGRESS', $4)`, scope, key, requestHash, s.clock().Add(idempotencyTTL)); err != nil {
			return nil, false, err
		}
		return nil, false, nil
	}
	if storedHash != requestHash {
		return nil, false, wallet.ErrIdempotencyConflict
	}
	switch status {
	case "SUCCEEDED":
		return body, true, nil
	case "FAILED":
		if _, err := tx.ExecContext(ctx, `UPDATE idempotency_records SET status = 'IN_PROGRESS', updated_at = CURRENT_TIMESTAMP
WHERE scope = $1 AND idempotency_key = $2`, scope, key); err != nil {
			return nil, false, err
		}
		return nil, false, nil
	default:
		return nil, false, wallet.ErrIdempotencyInProgress
	}
}

// replayAppliedCredit answers a retry whose credit is already in the ledger: the response is built from
// the stored after-balance, the cache is finalized with it, and nothing is credited again.
func (s *WalletStore) replayAppliedCredit(tx *sql.Tx, ctx context.Context, scope, key string, after int64) (wallet.WalletView, error) {
	view := wallet.WalletView{BalanceCent: after}
	body, err := json.Marshal(view)
	if err != nil {
		return wallet.WalletView{}, err
	}
	if err := s.finalizeWalletIdempotency(tx, ctx, scope, key, body); err != nil {
		return wallet.WalletView{}, err
	}
	if err := tx.Commit(); err != nil {
		return wallet.WalletView{}, err
	}
	return view, nil
}

func (s *WalletStore) finalizeWalletIdempotency(tx *sql.Tx, ctx context.Context, scope, key string, body []byte) error {
	_, err := tx.ExecContext(ctx, `UPDATE idempotency_records
SET status = 'SUCCEEDED', response_code = 0, response_body = $3, updated_at = CURRENT_TIMESTAMP
WHERE scope = $1 AND idempotency_key = $2`, scope, key, body)
	return err
}

// walletBalanceForUpdate returns the balance with the wallet row locked, creating a zero wallet when
// the user has none.
//
// The upsert is the fix for a lost update that this module shipped with. `SELECT ... FOR UPDATE` locks
// nothing when the row does not exist, so the previous two-step version let two concurrent first
// credits both read zero; because the credit then wrote an absolute balance, the second transaction
// overwrote the first while the ledger recorded both. Taking the lock in the same statement that
// creates the row removes the window entirely: whatever this call returns is a balance the caller may
// build on.
//
// A user that does not exist fails the foreign key. That is reported as a missing wallet (404) rather
// than as an internal error, because a wallet cannot exist without its account.
func (s *WalletStore) walletBalanceForUpdate(tx *sql.Tx, ctx context.Context, userID int64) (int64, error) {
	var balance int64
	err := tx.QueryRowContext(ctx, `INSERT INTO wallet_accounts (user_id, balance_cents)
VALUES ($1, 0)
ON CONFLICT (user_id) DO UPDATE SET user_id = EXCLUDED.user_id
RETURNING balance_cents`, userID).Scan(&balance)
	if err != nil {
		if isForeignKeyViolation(err) {
			return 0, wallet.ErrWalletNotFound
		}
		return 0, err
	}
	return balance, nil
}

// creditWalletLocked adds amount to a wallet the caller has already locked and returns the balance the
// database computed.
//
// The arithmetic happens in the statement, not in Go: `balance_cents = balance_cents + $2` cannot lose
// a concurrent credit even if the caller's earlier read were stale, and RETURNING is the authoritative
// after-value for the ledger row. The non-negative CHECK on the column stays the last line of defence.
func (s *WalletStore) creditWalletLocked(tx *sql.Tx, ctx context.Context, userID, amount int64) (int64, error) {
	var after int64
	if err := tx.QueryRowContext(ctx, `UPDATE wallet_accounts
SET balance_cents = balance_cents + $2, version = version + 1, updated_at = CURRENT_TIMESTAMP
WHERE user_id = $1
RETURNING balance_cents`, userID, amount).Scan(&after); err != nil {
		return 0, err
	}
	return after, nil
}

// walletBalance reads the balance without locking, creating the zero wallet when the user has none.
//
// A read must not take a write lock and must not rewrite the row on every call, so this path inserts
// only when the wallet is missing and re-reads afterwards: a concurrent creator's row is then observed
// instead of being reported as zero.
func (s *WalletStore) walletBalance(tx *sql.Tx, ctx context.Context, userID int64) (int64, error) {
	var balance int64
	err := tx.QueryRowContext(ctx, `SELECT balance_cents FROM wallet_accounts WHERE user_id = $1`, userID).Scan(&balance)
	if err == nil {
		return balance, nil
	}
	if !errors.Is(err, sql.ErrNoRows) {
		return 0, err
	}
	if _, err := tx.ExecContext(ctx, `INSERT INTO wallet_accounts (user_id, balance_cents) VALUES ($1, 0)
ON CONFLICT (user_id) DO NOTHING`, userID); err != nil {
		if isForeignKeyViolation(err) {
			return 0, wallet.ErrWalletNotFound
		}
		return 0, err
	}
	if err := tx.QueryRowContext(ctx, `SELECT balance_cents FROM wallet_accounts WHERE user_id = $1`, userID).Scan(&balance); err != nil {
		return 0, err
	}
	return balance, nil
}

// ledgerResult returns the balance a previously applied ledger row produced for this idempotency key.
//
// The ledger is the durable record of a credit; idempotency_records is a 24-hour response cache. When
// the cache has expired, a retry must still be answered with the result the first request produced and
// must not credit again - reading the ledger is what makes that true, instead of letting the retry
// reach the ledger's unique index and fail with a duplicate-key error for a request that succeeded.
func (s *WalletStore) ledgerResult(tx *sql.Tx, ctx context.Context, ledgerKey string) (int64, bool, error) {
	var after int64
	err := tx.QueryRowContext(ctx, `SELECT balance_after_cents FROM wallet_transactions WHERE idempotency_key = $1`, ledgerKey).Scan(&after)
	if errors.Is(err, sql.ErrNoRows) {
		return 0, false, nil
	}
	if err != nil {
		return 0, false, err
	}
	return after, true, nil
}

// isForeignKeyViolation reports whether an error is a missing referenced row (SQLSTATE 23503): for this
// store it means the account behind the wallet does not exist.
func isForeignKeyViolation(err error) bool {
	if err == nil {
		return false
	}
	var state interface{ SQLState() string }
	if errors.As(err, &state) {
		return state.SQLState() == "23503"
	}
	return strings.Contains(err.Error(), "23503")
}

// Wallet returns the balance view, auto-creating a zero wallet when the
// user has none.
func (s *WalletStore) Wallet(ctx context.Context, userID int64) (wallet.WalletView, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return wallet.WalletView{}, err
	}
	defer func() { _ = tx.Rollback() }()

	balance, err := s.walletBalance(tx, ctx, userID)
	if err != nil {
		return wallet.WalletView{}, err
	}
	if err := tx.Commit(); err != nil {
		return wallet.WalletView{}, err
	}
	return wallet.WalletView{BalanceCent: balance}, nil
}

// TopUp credits the amount and writes the TOP_UP ledger row inside one
// transaction. A replayed key returns the unchanged wallet view.
func (s *WalletStore) TopUp(ctx context.Context, command wallet.TopUpCommand) (wallet.WalletView, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return wallet.WalletView{}, err
	}
	defer func() { _ = tx.Rollback() }()

	scope := fmt.Sprintf("wallet:%d:top-up", command.UserID)
	replay, replayed, err := s.claimWalletIdempotency(tx, ctx, scope, command.IdempotencyKey, command.RequestHash)
	if err != nil {
		return wallet.WalletView{}, err
	}
	if replayed {
		var view wallet.WalletView
		if err := json.Unmarshal(replay, &view); err != nil {
			return wallet.WalletView{}, fmt.Errorf("decode idempotency replay: %w", err)
		}
		return view, nil
	}

	ledgerKey := fmt.Sprintf("topup:%d:%s", command.UserID, command.IdempotencyKey)
	if after, applied, err := s.ledgerResult(tx, ctx, ledgerKey); err != nil {
		return wallet.WalletView{}, err
	} else if applied {
		// This credit already happened; the response cache simply forgot it. Answering with the stored
		// result keeps the endpoint idempotent past the cache's lifetime.
		return s.replayAppliedCredit(tx, ctx, scope, command.IdempotencyKey, after)
	}

	balance, err := s.walletBalanceForUpdate(tx, ctx, command.UserID)
	if err != nil {
		return wallet.WalletView{}, err
	}
	after, err := s.creditWalletLocked(tx, ctx, command.UserID, command.AmountCent)
	if err != nil {
		return wallet.WalletView{}, err
	}
	if _, err := tx.ExecContext(ctx, `INSERT INTO wallet_transactions
    (user_id, transaction_type, amount_cents, balance_before_cents, balance_after_cents, idempotency_key)
VALUES ($1, 'TOP_UP', $2, $3, $4, $5)`,
		command.UserID, command.AmountCent, balance, after, ledgerKey); err != nil {
		return wallet.WalletView{}, err
	}

	view := wallet.WalletView{BalanceCent: after}
	body, err := json.Marshal(view)
	if err != nil {
		return wallet.WalletView{}, err
	}
	if err := s.finalizeWalletIdempotency(tx, ctx, scope, command.IdempotencyKey, body); err != nil {
		return wallet.WalletView{}, err
	}
	if err := tx.Commit(); err != nil {
		return wallet.WalletView{}, err
	}
	return view, nil
}

// ListTransactions returns one page of the user's ledger, newest first.
func (s *WalletStore) ListTransactions(ctx context.Context, filter wallet.EntryFilter) (wallet.EntryPage, error) {
	const filterSQL = `user_id = $1
  AND ($2 = '' OR transaction_type = $2)`
	const pageQuery = `SELECT id, transaction_type, amount_cents, balance_before_cents, balance_after_cents,
COALESCE((SELECT order_no FROM charging_orders o WHERE o.id = t.order_id), '') AS order_no,
idempotency_key, created_at
FROM wallet_transactions t
WHERE ` + filterSQL + `
ORDER BY created_at DESC, id DESC
LIMIT $3 OFFSET $4`
	const countQuery = `SELECT count(*) FROM wallet_transactions WHERE ` + filterSQL

	offset := (filter.Page - 1) * filter.PageSize
	args := []any{filter.UserID, filter.Type}

	rows, err := s.db.QueryContext(ctx, pageQuery, append(args, filter.PageSize, offset)...)
	if err != nil {
		return wallet.EntryPage{}, err
	}
	defer rows.Close()

	page := wallet.EntryPage{Meta: wallet.PageMeta{Page: filter.Page, PageSize: filter.PageSize}}
	for rows.Next() {
		var entry wallet.Entry
		if err := rows.Scan(&entry.ID, &entry.TransactionType, &entry.AmountCent,
			&entry.BalanceBeforeCent, &entry.BalanceAfterCent, &entry.OrderNo,
			&entry.IdempotencyKey, &entry.CreatedAt); err != nil {
			return wallet.EntryPage{}, err
		}
		entry.CreatedAt = entry.CreatedAt.UTC()
		page.Items = append(page.Items, entry)
	}
	if err := rows.Err(); err != nil {
		return wallet.EntryPage{}, err
	}
	if err := s.db.QueryRowContext(ctx, countQuery, args...).Scan(&page.Meta.Total); err != nil {
		return wallet.EntryPage{}, err
	}
	return page, nil
}

// RefundOrder returns a completed order's settled amount to the wallet
// (admin action, BR-11): the REFUND ledger row, the wallet credit and the
// order's payment-state revert (paid_cents cleared, status PENDING) commit
// together, with the audit trail in the same transaction.
func (s *WalletStore) RefundOrder(ctx context.Context, command wallet.RefundCommand) (wallet.WalletView, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return wallet.WalletView{}, err
	}
	defer func() { _ = tx.Rollback() }()

	scope := fmt.Sprintf("wallet:refund:%s", command.OrderNo)
	replay, replayed, err := s.claimWalletIdempotency(tx, ctx, scope, command.IdempotencyKey, command.RequestHash)
	if err != nil {
		return wallet.WalletView{}, err
	}
	if replayed {
		var view wallet.WalletView
		if err := json.Unmarshal(replay, &view); err != nil {
			return wallet.WalletView{}, fmt.Errorf("decode idempotency replay: %w", err)
		}
		return view, nil
	}

	// The ledger key identifies the REFUND REQUEST, not the order: it carries the order number and the
	// caller's idempotency key. A retry of the same request - before or after the response cache expires
	// - returns the refund that already happened; a different key is a different request, which the
	// order's own payment state rejects with "nothing to refund". Carrying only the order number would
	// make a later, unrelated refund request look like a success, which is the wrong answer for an
	// operator's books.
	ledgerKey := fmt.Sprintf("refund:%s:%s", command.OrderNo, command.IdempotencyKey)
	if after, applied, err := s.ledgerResult(tx, ctx, ledgerKey); err != nil {
		return wallet.WalletView{}, err
	} else if applied {
		return s.replayAppliedCredit(tx, ctx, scope, command.IdempotencyKey, after)
	}

	var orderID, userID, paidCents int64
	var orderStatus, paymentStatus string
	err = tx.QueryRowContext(ctx, `SELECT id, user_id, paid_cents, status, payment_status FROM charging_orders
WHERE order_no = $1 FOR UPDATE`, command.OrderNo).Scan(&orderID, &userID, &paidCents, &orderStatus, &paymentStatus)
	if errors.Is(err, sql.ErrNoRows) {
		// The order does not exist. wallet.ErrOrderNotFound is deliberately distinct from
		// wallet.ErrOrderNotRefundable: the caller cannot fix a missing order by looking at the order
		// state, which is why the contract asks for 404 here.
		return wallet.WalletView{}, wallet.ErrOrderNotFound
	}
	if err != nil {
		return wallet.WalletView{}, err
	}
	if orderStatus != "COMPLETED" || paidCents <= 0 || paymentStatus == "PENDING" {
		return wallet.WalletView{}, wallet.ErrOrderNotRefundable
	}

	balance, err := s.walletBalanceForUpdate(tx, ctx, userID)
	if err != nil {
		return wallet.WalletView{}, err
	}
	after, err := s.creditWalletLocked(tx, ctx, userID, paidCents)
	if err != nil {
		return wallet.WalletView{}, err
	}
	if _, err := tx.ExecContext(ctx, `INSERT INTO wallet_transactions
    (user_id, order_id, transaction_type, amount_cents, balance_before_cents, balance_after_cents, idempotency_key)
VALUES ($1, $2, 'REFUND', $3, $4, $5, $6)`,
		userID, orderID, paidCents, balance, after, ledgerKey); err != nil {
		return wallet.WalletView{}, err
	}
	if _, err := tx.ExecContext(ctx, `UPDATE charging_orders
SET paid_cents = 0, payment_status = 'PENDING', version = version + 1, updated_at = CURRENT_TIMESTAMP
WHERE id = $1`, orderID); err != nil {
		return wallet.WalletView{}, err
	}
	if _, err := tx.ExecContext(ctx, `INSERT INTO operation_logs
    (actor_type, actor_id, action, resource_type, resource_id, request_id, payload)
VALUES ('ADMIN', $1, 'wallet.refund', 'order', $2, $3, $4::jsonb)`,
		fmt.Sprintf("%d", command.AdminID), command.OrderNo, command.TraceID,
		fmt.Sprintf(`{"refundCent":%d,"orderNo":%q}`, paidCents, command.OrderNo)); err != nil {
		return wallet.WalletView{}, err
	}

	view := wallet.WalletView{BalanceCent: after}
	body, err := json.Marshal(view)
	if err != nil {
		return wallet.WalletView{}, err
	}
	if err := s.finalizeWalletIdempotency(tx, ctx, scope, command.IdempotencyKey, body); err != nil {
		return wallet.WalletView{}, err
	}
	if err := tx.Commit(); err != nil {
		return wallet.WalletView{}, err
	}
	return view, nil
}
