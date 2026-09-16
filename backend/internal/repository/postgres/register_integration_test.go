package postgres

import (
	"errors"
	"testing"

	"github.com/heguangV/charging-station-platform/backend/internal/auth"
)

// TestRegisterUserOnRealDatabase pins the registration write path: the account
// and its wallet are created together, the stored hash verifies against the
// password, and a phone that is already registered is refused with a business
// error and leaves nothing behind.
func TestRegisterUserOnRealDatabase(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewAccountStore(db)
	if err != nil {
		t.Fatalf("NewAccountStore() error = %v", err)
	}
	phone := "137" + digitsOnly(uniqueSuffix(t))[:8]
	password := "Dev-Password-01"

	hash, err := auth.HashPasswordWithIterations(password, 1000)
	if err != nil {
		t.Fatalf("hash password: %v", err)
	}

	user, err := store.RegisterUser(ctx, phone, "注册用户", hash)
	if err != nil {
		t.Fatalf("RegisterUser() error = %v", err)
	}
	if user.ID < 1 || user.Phone != phone || user.DisplayName != "注册用户" || user.Status != auth.StatusActive {
		t.Fatalf("registered user = %#v", user)
	}
	if !auth.VerifyPassword(password, user.PasswordHash) {
		t.Fatal("the stored hash does not verify against the password")
	}

	// The wallet exists and is empty, so the first top-up has a row to credit.
	var balance int64
	if err := db.QueryRowContext(ctx, `SELECT balance_cents FROM wallet_accounts WHERE user_id = $1`, user.ID).
		Scan(&balance); err != nil {
		t.Fatalf("registered user has no wallet: %v", err)
	}
	if balance != 0 {
		t.Fatalf("new wallet balance = %d, want 0", balance)
	}

	// The same phone again: the unique index answers, and no second user or
	// wallet appears.
	_, err = store.RegisterUser(ctx, phone, "另一个名字", hash)
	if !errors.Is(err, auth.ErrAccountExists) {
		t.Fatalf("duplicate registration error = %v, want ErrAccountExists", err)
	}
	var users, wallets int64
	if err := db.QueryRowContext(ctx, `SELECT count(*) FROM user_accounts WHERE phone = $1`, phone).Scan(&users); err != nil {
		t.Fatalf("count users: %v", err)
	}
	if err := db.QueryRowContext(ctx, `SELECT count(*) FROM wallet_accounts WHERE user_id = $1`, user.ID).Scan(&wallets); err != nil {
		t.Fatalf("count wallets: %v", err)
	}
	if users != 1 || wallets != 1 {
		t.Fatalf("users/wallets after a duplicate = %d/%d, want 1/1", users, wallets)
	}
	// The refused attempt must not have renamed the account either.
	var displayName string
	if err := db.QueryRowContext(ctx, `SELECT display_name FROM user_accounts WHERE phone = $1`, phone).Scan(&displayName); err != nil {
		t.Fatalf("read display name: %v", err)
	}
	if displayName != "注册用户" {
		t.Fatalf("display name = %q after a refused duplicate", displayName)
	}
}

// digitsOnly strips the punctuation uniqueSuffix keeps, so the value can be used
// as a phone number.
func digitsOnly(value string) string {
	out := make([]rune, 0, len(value))
	for _, r := range value {
		if r >= '0' && r <= '9' {
			out = append(out, r)
		}
	}
	return string(out)
}
