package postgres

import (
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
	"strconv"

	"github.com/heguangV/charging-station-platform/backend/internal/admin"
)

// Manual user archiving, against PostgreSQL.
//
// An archived account is born exactly the way a self-registered one is (UC-U-01):
// ACTIVE, a zero-balance wallet, no password, and the registration default for
// a name the operator did not type. The roster page is one INSERT over arrays,
// for the same reason the charger batch is: a phone that already exists aborts
// the single statement, and the transaction rolls back with nothing created -
// not the entries before it, not the entries after it.

// CreateUser archives one account, under audit and one idempotency key.
func (s *AdminStore) CreateUser(ctx context.Context, command admin.CreateUserCommand) (admin.UserRecord, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return admin.UserRecord{}, err
	}
	defer func() { _ = tx.Rollback() }()

	scope := fmt.Sprintf("admin:%d:user:create", command.AdminID)
	replay, replayed, err := s.claimIdempotency(tx, ctx, scope, command.IdempotencyKey, command.RequestHash)
	if err != nil {
		return admin.UserRecord{}, err
	}
	if replayed {
		// The account already exists because this exact request already ran.
		// The recorded answer is returned unchanged, so a retried submission
		// neither fails nor fabricates a second account.
		var record admin.UserRecord
		if err := json.Unmarshal(replay, &record); err != nil {
			return admin.UserRecord{}, fmt.Errorf("decode idempotency replay: %w", err)
		}
		return record, nil
	}

	created, err := insertUserDrafts(tx, ctx, []admin.UserDraft{command.User})
	if err != nil {
		return admin.UserRecord{}, err
	}
	if len(created) != 1 {
		return admin.UserRecord{}, admin.ErrDuplicateUserPhone
	}

	// The audit trail carries the masked phone: enough for the operator who
	// typed it to recognise the entry, never the full number (security
	// baseline: no sensitive fields in logs).
	if err := s.appendAudit(tx, ctx, command.AdminID, "user.create", "user",
		strconv.FormatInt(created[0].ID, 10), command.TraceID,
		map[string]any{"phone": admin.MaskPhone(created[0].Phone), "displayName": created[0].DisplayName}); err != nil {
		return admin.UserRecord{}, err
	}

	body, err := json.Marshal(created[0])
	if err != nil {
		return admin.UserRecord{}, err
	}
	if err := s.finalizeIdempotency(tx, ctx, scope, command.IdempotencyKey, body); err != nil {
		return admin.UserRecord{}, err
	}
	if err := tx.Commit(); err != nil {
		return admin.UserRecord{}, err
	}
	return created[0], nil
}

// CreateUsers archives a page of a roster, under audit and one idempotency key.
func (s *AdminStore) CreateUsers(ctx context.Context, command admin.CreateUsersCommand) (admin.UserBatchResult, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return admin.UserBatchResult{}, err
	}
	defer func() { _ = tx.Rollback() }()

	scope := fmt.Sprintf("admin:%d:user:batch", command.AdminID)
	replay, replayed, err := s.claimIdempotency(tx, ctx, scope, command.IdempotencyKey, command.RequestHash)
	if err != nil {
		return admin.UserBatchResult{}, err
	}
	if replayed {
		var result admin.UserBatchResult
		if err := json.Unmarshal(replay, &result); err != nil {
			return admin.UserBatchResult{}, fmt.Errorf("decode idempotency replay: %w", err)
		}
		return result, nil
	}

	created, err := insertUserDrafts(tx, ctx, command.Users)
	if err != nil {
		return admin.UserBatchResult{}, err
	}
	// A RETURNING that produced fewer rows than were sent means the insert did
	// not apply everything; committing would hide that, so the batch is refused.
	if len(created) != len(command.Users) {
		return admin.UserBatchResult{}, fmt.Errorf("postgres: created %d of %d users", len(created), len(command.Users))
	}

	// The audit records the count and the masked phones. A roster page is a
	// bounded payload, and "which accounts did that page create" is the
	// question an operator asks when the count disagrees with the roster.
	masked := make([]string, 0, len(created))
	for _, record := range created {
		masked = append(masked, admin.MaskPhone(record.Phone))
	}
	if err := s.appendAudit(tx, ctx, command.AdminID, "user.batch-create", "user_batch",
		"", command.TraceID,
		map[string]any{"userCount": len(created), "phones": masked}); err != nil {
		return admin.UserBatchResult{}, err
	}

	result := admin.UserBatchResult{
		UserCount: int64(len(created)),
		Created:   created,
	}
	body, err := json.Marshal(result)
	if err != nil {
		return admin.UserBatchResult{}, err
	}
	if err := s.finalizeIdempotency(tx, ctx, scope, command.IdempotencyKey, body); err != nil {
		return admin.UserBatchResult{}, err
	}
	if err := tx.Commit(); err != nil {
		return admin.UserBatchResult{}, err
	}
	return result, nil
}

// insertUserDrafts writes the accounts and their zero-balance wallets and
// returns the records in arrival order. The unique index on phone is the
// authority for a phone that already exists: it is a business answer (409),
// not an internal error, and the failed statement has already taken the whole
// batch with it.
func insertUserDrafts(tx *sql.Tx, ctx context.Context, drafts []admin.UserDraft) ([]admin.UserRecord, error) {
	phones := make([]string, 0, len(drafts))
	names := make([]string, 0, len(drafts))
	for _, draft := range drafts {
		phones = append(phones, draft.Phone)
		// An unnamed draft takes the registration default, so an archived
		// account is indistinguishable from a self-registered one in every list.
		name := draft.DisplayName
		if name == "" {
			name = admin.DefaultUserDisplayName(draft.Phone)
		}
		names = append(names, name)
	}

	rows, err := tx.QueryContext(ctx, `INSERT INTO user_accounts (phone, display_name, password_hash, status)
SELECT phone, display_name, '', 'ACTIVE'
FROM unnest($1::text[], $2::text[]) AS drafted(phone, display_name)
RETURNING id, phone, display_name, status`, phones, names)
	if err != nil {
		if isUniqueViolation(err) {
			return nil, fmt.Errorf("%w: a phone in this request is already registered",
				admin.ErrDuplicateUserPhone)
		}
		return nil, err
	}
	defer rows.Close()

	created := make([]admin.UserRecord, 0, len(drafts))
	ids := make([]int64, 0, len(drafts))
	for rows.Next() {
		var record admin.UserRecord
		if err := rows.Scan(&record.ID, &record.Phone, &record.DisplayName, &record.Status); err != nil {
			return nil, err
		}
		created = append(created, record)
		ids = append(ids, record.ID)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	if len(created) == 0 {
		return created, nil
	}

	// The wallet is part of the account, not a side effect: every account this
	// statement created gets one, at the same zero balance UC-U-01 starts with.
	if _, err := tx.ExecContext(ctx, `INSERT INTO wallet_accounts (user_id, balance_cents)
SELECT user_id, 0 FROM unnest($1::bigint[]) AS drafted(user_id)
ON CONFLICT (user_id) DO NOTHING`, ids); err != nil {
		return nil, err
	}
	return created, nil
}
