package postgres

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"strings"
	"testing"

	"github.com/heguangV/charging-station-platform/backend/internal/admin"
)

// Manual user archiving against a real PostgreSQL.
//
// The unit tests prove what the endpoint accepts. What can only be proved
// here is the property the feature exists for: an archived account is born a
// self-registered one (wallet, default name, no credentials), a page either
// creates every account it was given or none of them, and a retry creates
// nothing twice.

// seedUserArchivePhone derives a fresh phone from the unique test suffix so
// every run archives its own accounts.
func seedUserArchivePhone(suffix string, index int) string {
	digits := strings.ReplaceAll(suffix, ".", "")
	digits = digits[len(digits)-8:]
	return fmt.Sprintf("139%s%02d", digits, index)
}

// userArchiveCleanup removes everything a user-archive test created, in an
// order the foreign keys accept.
func userArchiveCleanup(db *sql.DB, ctx context.Context, phones []string) func() {
	return func() {
		_, _ = db.ExecContext(ctx, `DELETE FROM wallet_accounts WHERE user_id IN (SELECT id FROM user_accounts WHERE phone = ANY($1))`, phones)
		_, _ = db.ExecContext(ctx, `DELETE FROM operation_logs WHERE action IN ('user.create', 'user.batch-create')`)
		_, _ = db.ExecContext(ctx, `DELETE FROM idempotency_records WHERE scope LIKE 'admin:%:user:create' OR scope LIKE 'admin:%:user:batch'`)
		_, _ = db.ExecContext(ctx, `DELETE FROM user_accounts WHERE phone = ANY($1)`, phones)
	}
}

func TestAdminCreateUserArchivesOneAccountAndAudits(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewAdminStore(db)
	if err != nil {
		t.Fatalf("NewAdminStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	phone := seedUserArchivePhone(suffix, 1)
	t.Cleanup(userArchiveCleanup(db, ctx, []string{phone, seedUserArchivePhone(suffix, 2)}))

	created, err := store.CreateUser(ctx, admin.CreateUserCommand{
		AdminID: 9, User: admin.UserDraft{Phone: phone, DisplayName: "老王"},
		IdempotencyKey: "ua-one-" + suffix, RequestHash: "h", TraceID: "trace-archive",
	})
	if err != nil {
		t.Fatalf("CreateUser() error = %v", err)
	}
	if created.Status != admin.UserStatusActive || created.BalanceCent != 0 || created.DisplayName != "老王" {
		t.Fatalf("created = %#v", created)
	}

	// The wallet is part of the account: zero balance, like UC-U-01 starts.
	var balance int64
	if err := db.QueryRowContext(ctx,
		`SELECT balance_cents FROM wallet_accounts WHERE user_id = $1`, created.ID).Scan(&balance); err != nil || balance != 0 {
		t.Fatalf("wallet = %d (%v)", balance, err)
	}

	// No credential is minted: the account signs in through SMS.
	var passwordHash string
	if err := db.QueryRowContext(ctx,
		`SELECT password_hash FROM user_accounts WHERE id = $1`, created.ID).Scan(&passwordHash); err != nil || passwordHash != "" {
		t.Fatalf("password hash = %q (%v)", passwordHash, err)
	}

	// The audit trail carries the masked phone, never the full number.
	var payloadPhone string
	if err := db.QueryRowContext(ctx,
		`SELECT payload->>'phone' FROM operation_logs WHERE action = 'user.create' AND resource_id = $1`,
		strconvFormatInt64(created.ID)).Scan(&payloadPhone); err != nil {
		t.Fatalf("audit: %v", err)
	}
	if payloadPhone != admin.MaskPhone(phone) {
		t.Fatalf("audit phone = %q, want masked", payloadPhone)
	}

	// The same key replays the stored answer without a second account.
	replay, err := store.CreateUser(ctx, admin.CreateUserCommand{
		AdminID: 9, User: admin.UserDraft{Phone: phone, DisplayName: "老王"},
		IdempotencyKey: "ua-one-" + suffix, RequestHash: "h", TraceID: "trace-archive",
	})
	if err != nil || replay.ID != created.ID {
		t.Fatalf("replay = %#v, %v", replay, err)
	}

	// A phone that already exists is a conflict, not a warning.
	if _, err := store.CreateUser(ctx, admin.CreateUserCommand{
		AdminID: 9, User: admin.UserDraft{Phone: phone},
		IdempotencyKey: "ua-dup-" + suffix, RequestHash: "h", TraceID: "trace-archive",
	}); !errors.Is(err, admin.ErrDuplicateUserPhone) {
		t.Fatalf("duplicate error = %v", err)
	}
}

func TestAdminCreateUserTakesTheRegistrationDefault(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewAdminStore(db)
	if err != nil {
		t.Fatalf("NewAdminStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	phone := seedUserArchivePhone(suffix, 2)
	t.Cleanup(userArchiveCleanup(db, ctx, []string{phone}))

	created, err := store.CreateUser(ctx, admin.CreateUserCommand{
		AdminID: 9, User: admin.UserDraft{Phone: phone},
		IdempotencyKey: "ua-default-" + suffix, RequestHash: "h", TraceID: "trace-archive",
	})
	if err != nil {
		t.Fatalf("CreateUser() error = %v", err)
	}
	if created.DisplayName != admin.DefaultUserDisplayName(phone) {
		t.Fatalf("display name = %q, want %q", created.DisplayName, admin.DefaultUserDisplayName(phone))
	}
}

func TestAdminCreateUsersBatchCreatesEveryAccountAndAudits(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewAdminStore(db)
	if err != nil {
		t.Fatalf("NewAdminStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	phones := []string{
		seedUserArchivePhone(suffix, 11),
		seedUserArchivePhone(suffix, 12),
		seedUserArchivePhone(suffix, 13),
	}
	t.Cleanup(userArchiveCleanup(db, ctx, phones))

	page := admin.CreateUsersCommand{
		AdminID: 9,
		Users: []admin.UserDraft{
			{Phone: phones[0], DisplayName: "批量一"},
			{Phone: phones[1]},
			{Phone: phones[2], DisplayName: "批量三"},
		},
		IdempotencyKey: "ub-page-" + suffix, RequestHash: "h", TraceID: "trace-archive",
	}

	result, err := store.CreateUsers(ctx, page)
	if err != nil {
		t.Fatalf("CreateUsers() error = %v", err)
	}
	if result.UserCount != 3 || len(result.Created) != 3 {
		t.Fatalf("result = %#v", result)
	}
	// The unnamed entry takes the registration default.
	if result.Created[1].DisplayName != admin.DefaultUserDisplayName(phones[1]) {
		t.Fatalf("default name = %q", result.Created[1].DisplayName)
	}
	for _, record := range result.Created {
		var balance int64
		if err := db.QueryRowContext(ctx,
			`SELECT balance_cents FROM wallet_accounts WHERE user_id = $1`, record.ID).Scan(&balance); err != nil || balance != 0 {
			t.Fatalf("wallet for %d = %d (%v)", record.ID, balance, err)
		}
	}

	// The same key replays the stored answer in the same order.
	replay, err := store.CreateUsers(ctx, page)
	if err != nil || replay.UserCount != 3 ||
		replay.Created[0].ID != result.Created[0].ID || replay.Created[2].ID != result.Created[2].ID {
		t.Fatalf("replay = %#v, %v", replay, err)
	}

	// One audit row records the page, with masked phones.
	var audits int64
	if err := db.QueryRowContext(ctx,
		`SELECT count(*) FROM operation_logs WHERE action = 'user.batch-create'
		  AND payload->>'phones' LIKE '%' || $1 || '%'`,
		admin.MaskPhone(phones[0])).Scan(&audits); err != nil || audits != 1 {
		t.Fatalf("batch audits = %d (%v)", audits, err)
	}
}

func TestAdminCreateUsersBatchCreatesNothingWhenOnePhoneExists(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewAdminStore(db)
	if err != nil {
		t.Fatalf("NewAdminStore() error = %v", err)
	}
	suffix := uniqueSuffix(t)
	phones := []string{
		seedUserArchivePhone(suffix, 21),
		seedUserArchivePhone(suffix, 22),
		seedUserArchivePhone(suffix, 23),
	}
	t.Cleanup(userArchiveCleanup(db, ctx, phones))

	seed := admin.CreateUsersCommand{
		AdminID:        9,
		Users:          []admin.UserDraft{{Phone: phones[0]}},
		IdempotencyKey: "ub-seed-" + suffix, RequestHash: "h", TraceID: "trace-archive",
	}
	if _, err := store.CreateUsers(ctx, seed); err != nil {
		t.Fatalf("seed page error = %v", err)
	}

	// The page carries one account that already exists; the whole page is
	// refused and nothing beyond the collision is created.
	conflict := admin.CreateUsersCommand{
		AdminID: 9,
		Users: []admin.UserDraft{
			{Phone: phones[1]},
			{Phone: phones[0]},
			{Phone: phones[2]},
		},
		IdempotencyKey: "ub-conflict-" + suffix, RequestHash: "h", TraceID: "trace-archive",
	}
	if _, err := store.CreateUsers(ctx, conflict); !errors.Is(err, admin.ErrDuplicateUserPhone) {
		t.Fatalf("conflict error = %v", err)
	}

	var total int64
	if err := db.QueryRowContext(ctx,
		`SELECT count(*) FROM user_accounts WHERE phone = ANY($1)`, phones).Scan(&total); err != nil || total != 1 {
		t.Fatalf("account rows = %d (%v), want 1", total, err)
	}
}
