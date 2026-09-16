package postgres

import (
	"context"
	"database/sql"
	"errors"
	"strings"

	"github.com/heguangV/charging-station-platform/backend/internal/auth"
)

// AccountStore implements auth.AccountReader against user_accounts and
// admin_accounts. Every query is parameterized.
type AccountStore struct {
	db *sql.DB
}

// NewAccountStore binds the store to a connection pool.
func NewAccountStore(db *sql.DB) (*AccountStore, error) {
	if db == nil {
		return nil, errors.New("postgres: account store requires a database")
	}
	return &AccountStore{db: db}, nil
}

// FindUserByAccount resolves a user by exact phone or email match. Unknown
// accounts return (nil, nil) so login timing is equalized by the service.
func (s *AccountStore) FindUserByAccount(ctx context.Context, account string) (*auth.UserAccount, error) {
	const query = `SELECT id, phone, email, display_name, password_hash, status
FROM user_accounts
WHERE phone = $1 OR email = $1
LIMIT 1`

	var user auth.UserAccount
	var phone, email sql.NullString
	err := s.db.QueryRowContext(ctx, query, account).Scan(
		&user.ID, &phone, &email, &user.DisplayName, &user.PasswordHash, &user.Status,
	)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	user.Phone = phone.String
	user.Email = email.String
	return &user, nil
}

// FindAdminByUsername resolves an administrator by exact username match.
func (s *AccountStore) FindAdminByUsername(ctx context.Context, username string) (*auth.AdminAccount, error) {
	const query = `SELECT id, username, role, password_hash, status
FROM admin_accounts
WHERE username = $1
LIMIT 1`

	var admin auth.AdminAccount
	err := s.db.QueryRowContext(ctx, query, username).Scan(
		&admin.ID, &admin.Username, &admin.Role, &admin.PasswordHash, &admin.Status,
	)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	return &admin, nil
}

// FindAdminByID revalidates live administrator sessions against the current account state.
func (s *AccountStore) FindAdminByID(ctx context.Context, adminID int64) (*auth.AdminAccount, error) {
	const query = `SELECT id, username, role, password_hash, status
FROM admin_accounts WHERE id = $1 LIMIT 1`
	var account auth.AdminAccount
	if err := s.db.QueryRowContext(ctx, query, adminID).Scan(
		&account.ID, &account.Username, &account.Role, &account.PasswordHash, &account.Status,
	); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return nil, nil
		}
		return nil, err
	}
	return &account, nil
}

// AccountMutationAdapter implements the A-line auth.AccountMutation port
// over user_accounts. Delivered by A-01 per the two-track split: the port
// lives in internal/auth, the PostgreSQL adapter lives here (B-02).
//
// Transaction boundary (A-01 review, B line):
//
// Every method here is a single statement, and in PostgreSQL a single statement is already one
// transaction, applied or not applied as a whole. Wrapping it in BeginTx/Commit would add a round
// trip and no atomicity, so the port's "every method must run inside a transaction" is satisfied by
// construction rather than by an explicit block. What no adapter can make atomic is the boundary the
// service draws across two stores: the status change commits here while session revocation happens in
// Redis. See docs/migration/a-01-persistence-review.md for that analysis and its consequences.
type AccountMutationAdapter struct {
	db *sql.DB
}

// NewAccountMutationAdapter binds the adapter to a connection pool.
func NewAccountMutationAdapter(db *sql.DB) (*AccountMutationAdapter, error) {
	if db == nil {
		return nil, errors.New("postgres: mutation adapter requires a database")
	}
	return &AccountMutationAdapter{db: db}, nil
}

// profileColumns is the projection every profile statement returns, so a column added to one path
// cannot be forgotten in the others.
const profileColumns = `id, phone, display_name, avatar_url, status, created_at`

// GetProfile returns the profile view; deleted accounts are not found.
//
// ProfileView.PhoneMasked carries the RAW phone from this store: masking is applied by the service
// (auth.MaskPhone) on the way out. The field name is therefore misleading, and a caller that forgets
// to mask leaks a phone number. It is left as it is because renaming the port field is an A-line
// change; the B-06 A-01 review records it. Do not return this value to a client unmasked.
func (s *AccountMutationAdapter) GetProfile(ctx context.Context, userID int64) (auth.ProfileView, error) {
	const query = `SELECT ` + profileColumns + `
FROM user_accounts WHERE id = $1 AND deleted_at IS NULL`
	var view auth.ProfileView
	var phone, avatar sql.NullString
	if err := s.db.QueryRowContext(ctx, query, userID).Scan(
		&view.ID, &phone, &view.DisplayName, &avatar, &view.Status, &view.RegisteredAt); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return auth.ProfileView{}, auth.ErrProfileNotFound
		}
		return auth.ProfileView{}, err
	}
	view.PhoneMasked = phone.String
	view.AvatarURL = avatar.String
	return view, nil
}

// UpdateProfile applies a nickname and/or avatar change and returns the stored view.
//
// One statement with COALESCE instead of one statement per field combination: a nil pointer means
// "leave this column alone", which is exactly what COALESCE($n, column) expresses, and the single
// statement cannot drift out of step with itself the way three near-identical ones can. The previous
// version also had a panic path - a call with neither field set reached the avatar branch and
// dereferenced a nil pointer - which the handler happened to prevent by rejecting such requests.
// A store should not depend on a caller for that, so it is refused here.
func (s *AccountMutationAdapter) UpdateProfile(ctx context.Context, userID int64, update auth.ProfileUpdate) (auth.ProfileView, error) {
	if update.DisplayName == nil && update.AvatarURL == nil {
		return auth.ProfileView{}, errors.New("postgres: profile update requires at least one field")
	}
	const query = `UPDATE user_accounts
SET display_name = COALESCE($2, display_name),
    avatar_url   = COALESCE($3, avatar_url),
    updated_at   = CURRENT_TIMESTAMP
WHERE id = $1 AND deleted_at IS NULL
RETURNING ` + profileColumns

	var view auth.ProfileView
	var phone, avatar sql.NullString
	if err := s.db.QueryRowContext(ctx, query, userID, update.DisplayName, update.AvatarURL).Scan(
		&view.ID, &phone, &view.DisplayName, &avatar, &view.Status, &view.RegisteredAt); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return auth.ProfileView{}, auth.ErrProfileNotFound
		}
		return auth.ProfileView{}, err
	}
	view.PhoneMasked = phone.String
	view.AvatarURL = avatar.String
	return view, nil
}

// DeleteAccount anonymizes the account in place (UC-U-05 申请注销): the
// phone and display name are replaced with irreversible placeholders, the
// password is dropped and the account is disabled. Returns false when the
// account was already deleted or never existed.
//
// All five fields change in one statement, and migration 0008 makes two of them invariants of a
// deleted row (deleted_at IS NULL OR status = 'DISABLED', deleted_at IS NULL OR password_hash = ”),
// so a future edit cannot half-delete an account and leave it able to log in.
//
// The anonymized phone is derived from the id, which is what keeps it unique without a second query:
// the row keeps its UNIQUE (phone) slot while releasing the real number, so the same person can
// register again and get a new account.
func (s *AccountMutationAdapter) DeleteAccount(ctx context.Context, userID int64) (bool, error) {
	tag, err := s.db.ExecContext(ctx, `UPDATE user_accounts
SET phone = 'deleted-' || id::text || '@invalid',
    display_name = '已注销用户',
    password_hash = '',
    avatar_url = '',
    status = 'DISABLED',
    deleted_at = CURRENT_TIMESTAMP,
    updated_at = CURRENT_TIMESTAMP
WHERE id = $1 AND deleted_at IS NULL`, userID)
	if err != nil {
		return false, err
	}
	affected, err := tag.RowsAffected()
	if err != nil {
		return false, err
	}
	return affected > 0, nil
}

// SetFrozen sets or clears the DISABLED status (BR-07). Returns false when
// the account is missing or already deleted.
//
// Unfreezing writes ACTIVE unconditionally. That is correct today because freeze is the only thing
// that writes DISABLED, and a deleted account cannot be unfrozen (deleted_at IS NULL guard). If a
// second reason to disable an account is ever introduced, this must record which reason disabled it
// rather than assuming it was a freeze.
func (s *AccountMutationAdapter) SetFrozen(ctx context.Context, userID int64, frozen bool) (bool, error) {
	status := "ACTIVE"
	if frozen {
		status = "DISABLED"
	}
	tag, err := s.db.ExecContext(ctx, `UPDATE user_accounts
SET status = $2, updated_at = CURRENT_TIMESTAMP
WHERE id = $1 AND deleted_at IS NULL`, userID, status)
	if err != nil {
		return false, err
	}
	affected, err := tag.RowsAffected()
	if err != nil {
		return false, err
	}
	return affected > 0, nil
}

var _ auth.AccountMutation = (*AccountMutationAdapter)(nil)

// EnsureUserWithWallet registers the user and the wallet when missing
// (UC-U-01) and returns the account either way. Both paths execute the same
// statements — an INSERT ... ON CONFLICT DO NOTHING for the user and for the
// wallet, then a SELECT — so a registered phone is not enumerable through
// response timing. Display name defaults to 用户 + the last four phone
// digits, balance starts at zero, and no password is set until the user
// creates one.
// RegisterUser creates a user with a password and its wallet in one
// transaction. The unique index on phone is the authority: a duplicate comes back
// as ErrAccountExists rather than as a driver error, and the transaction rolls
// back so a failed registration leaves neither a user nor a wallet behind.
// isUniqueViolation reports whether an error is a unique-index conflict
// (SQLSTATE 23505). For registration that means the phone already has an
// account, which is a business answer (409) rather than an internal error.
func isUniqueViolation(err error) bool {
	if err == nil {
		return false
	}
	var state interface{ SQLState() string }
	if errors.As(err, &state) {
		return state.SQLState() == "23505"
	}
	return strings.Contains(err.Error(), "23505")
}

func (s *AccountStore) RegisterUser(ctx context.Context, phone, displayName, passwordHash string) (auth.UserAccount, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return auth.UserAccount{}, err
	}
	defer func() { _ = tx.Rollback() }()

	var user auth.UserAccount
	var email sql.NullString
	err = tx.QueryRowContext(ctx, `INSERT INTO user_accounts (phone, display_name, password_hash, status)
VALUES ($1, $2, $3, 'ACTIVE')
RETURNING id, phone, email, display_name, password_hash, status`,
		phone, displayName, passwordHash).
		Scan(&user.ID, &user.Phone, &email, &user.DisplayName, &user.PasswordHash, &user.Status)
	if isUniqueViolation(err) {
		return auth.UserAccount{}, auth.ErrAccountExists
	}
	if err != nil {
		return auth.UserAccount{}, err
	}
	if _, err := tx.ExecContext(ctx, `INSERT INTO wallet_accounts (user_id, balance_cents)
VALUES ($1, 0) ON CONFLICT (user_id) DO NOTHING`, user.ID); err != nil {
		return auth.UserAccount{}, err
	}
	user.Email = email.String
	if err := tx.Commit(); err != nil {
		return auth.UserAccount{}, err
	}
	return user, nil
}

func (s *AccountStore) EnsureUserWithWallet(ctx context.Context, phone string) (auth.UserAccount, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return auth.UserAccount{}, err
	}
	defer func() { _ = tx.Rollback() }()

	displayName := "用户" + phone[len(phone)-4:]
	if _, err := tx.ExecContext(ctx, `INSERT INTO user_accounts (phone, display_name, password_hash, status)
VALUES ($1, $2, '', 'ACTIVE') ON CONFLICT (phone) DO NOTHING`, phone, displayName); err != nil {
		return auth.UserAccount{}, err
	}
	if _, err := tx.ExecContext(ctx, `INSERT INTO wallet_accounts (user_id, balance_cents)
SELECT id, 0 FROM user_accounts WHERE phone = $1 ON CONFLICT (user_id) DO NOTHING`, phone); err != nil {
		return auth.UserAccount{}, err
	}

	var user auth.UserAccount
	var email sql.NullString
	err = tx.QueryRowContext(ctx, `SELECT id, phone, email, display_name, password_hash, status
FROM user_accounts WHERE phone = $1`, phone).Scan(
		&user.ID, &user.Phone, &email, &user.DisplayName, &user.PasswordHash, &user.Status)
	if err != nil {
		return auth.UserAccount{}, err
	}
	user.Email = email.String
	if err := tx.Commit(); err != nil {
		return auth.UserAccount{}, err
	}
	return user, nil
}
