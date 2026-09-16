package postgres

import (
	"fmt"
	"testing"

	"github.com/heguangV/charging-station-platform/backend/internal/admin"
	"github.com/heguangV/charging-station-platform/backend/internal/auth"
)

func TestAdminAccountManagementRoundTrip(t *testing.T) {
	db, ctx := integrationDB(t)
	store, err := NewAdminStore(db)
	if err != nil {
		t.Fatalf("NewAdminStore() error = %v", err)
	}
	actorID := appealAdminActor(t, db, ctx)
	username := "operator_" + uniqueSuffix(t)
	password := "Initial-Password-01"
	hash, err := auth.HashPasswordWithIterations(password, 1000)
	if err != nil {
		t.Fatalf("hash password: %v", err)
	}

	created, err := store.CreateAdminAccount(ctx, admin.CreateAdminAccountCommand{
		ActorID: actorID, Username: username, PasswordHash: hash, Reason: "补充运营值班账号", RequestID: "account-create-test",
	})
	if err != nil {
		t.Fatalf("CreateAdminAccount() error = %v", err)
	}
	if created.Status != admin.AdminAccountEnabled || !created.MustChangePassword || created.Version != 1 {
		t.Fatalf("created account = %+v", created)
	}
	if len(created.Roles) != 1 || created.Roles[0] != auth.AdminRoleOperator {
		t.Fatalf("created roles = %v, want OPERATOR", created.Roles)
	}

	page, err := store.ListAdminAccounts(ctx, 1, 100)
	if err != nil {
		t.Fatalf("ListAdminAccounts() error = %v", err)
	}
	found := false
	for _, account := range page.Items {
		if account.ID == created.ID {
			found = true
		}
	}
	if !found {
		t.Fatalf("created account %d missing from list", created.ID)
	}

	disabled, err := store.SetAdminAccountStatus(ctx, admin.SetAdminAccountStatusCommand{
		ActorID: actorID, AccountID: created.ID, Status: admin.AdminAccountDisabled,
		Version: created.Version, Reason: "值班周期结束", RequestID: "account-disable-test",
	})
	if err != nil {
		t.Fatalf("SetAdminAccountStatus() error = %v", err)
	}
	if disabled.Status != admin.AdminAccountDisabled || disabled.Version != 2 {
		t.Fatalf("disabled account = %+v", disabled)
	}
	if _, err := store.SetAdminAccountStatus(ctx, admin.SetAdminAccountStatusCommand{
		ActorID: actorID, AccountID: created.ID, Status: admin.AdminAccountEnabled,
		Version: created.Version, Reason: "使用过期版本", RequestID: "account-stale-test",
	}); err != admin.ErrAdminAccountVersionConflict {
		t.Fatalf("stale status update error = %v, want version conflict", err)
	}

	var auditRows int64
	if err := db.QueryRowContext(ctx, `SELECT count(*) FROM operation_logs
WHERE resource_type = 'admin_account' AND resource_id = $1`, fmt.Sprintf("%d", created.ID)).Scan(&auditRows); err != nil {
		t.Fatalf("count account audits: %v", err)
	}
	if auditRows != 2 {
		t.Fatalf("admin account audit rows = %d, want 2", auditRows)
	}
}
