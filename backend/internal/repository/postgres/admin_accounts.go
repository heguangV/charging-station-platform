package postgres

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"

	"github.com/heguangV/charging-station-platform/backend/internal/admin"
)

func (s *AdminStore) ListAdminAccounts(ctx context.Context, page, pageSize int64) (admin.AdminAccountPage, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT id, username, role,
CASE status WHEN 'ACTIVE' THEN 1 ELSE 0 END, must_change_password, version
FROM admin_accounts ORDER BY created_at DESC, id DESC LIMIT $1 OFFSET $2`, pageSize, (page-1)*pageSize)
	if err != nil {
		return admin.AdminAccountPage{}, err
	}
	defer rows.Close()
	result := admin.AdminAccountPage{Meta: admin.PageMeta{Page: page, PageSize: pageSize}}
	for rows.Next() {
		var view admin.AdminAccountView
		var role string
		if err := rows.Scan(&view.ID, &view.Username, &role, &view.Status, &view.MustChangePassword, &view.Version); err != nil {
			return admin.AdminAccountPage{}, err
		}
		view.Roles = []string{role}
		result.Items = append(result.Items, view)
	}
	if err := rows.Err(); err != nil {
		return admin.AdminAccountPage{}, err
	}
	if err := s.db.QueryRowContext(ctx, `SELECT count(*) FROM admin_accounts`).Scan(&result.Meta.Total); err != nil {
		return admin.AdminAccountPage{}, err
	}
	return result, nil
}

func (s *AdminStore) CreateAdminAccount(ctx context.Context, command admin.CreateAdminAccountCommand) (admin.AdminAccountView, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return admin.AdminAccountView{}, err
	}
	defer func() { _ = tx.Rollback() }()

	var view admin.AdminAccountView
	var role string
	err = tx.QueryRowContext(ctx, `INSERT INTO admin_accounts
    (username, password_hash, role, status, must_change_password, version)
VALUES ($1, $2, 'OPERATOR', 'ACTIVE', TRUE, 1)
RETURNING id, username, role, 1, must_change_password, version`, command.Username, command.PasswordHash).
		Scan(&view.ID, &view.Username, &role, &view.Status, &view.MustChangePassword, &view.Version)
	if isUniqueViolation(err) {
		return admin.AdminAccountView{}, admin.ErrAdminAccountExists
	}
	if err != nil {
		return admin.AdminAccountView{}, err
	}
	view.Roles = []string{role}
	payload, _ := json.Marshal(map[string]any{"username": view.Username, "role": role, "reason": command.Reason})
	if _, err := tx.ExecContext(ctx, `INSERT INTO operation_logs
    (actor_type, actor_id, action, resource_type, resource_id, request_id, payload)
VALUES ('ADMIN', $1, 'admin_account.create', 'admin_account', $2, $3, $4::jsonb)`,
		fmt.Sprintf("%d", command.ActorID), fmt.Sprintf("%d", view.ID), command.RequestID, string(payload)); err != nil {
		return admin.AdminAccountView{}, err
	}
	if err := tx.Commit(); err != nil {
		return admin.AdminAccountView{}, err
	}
	return view, nil
}

func (s *AdminStore) SetAdminAccountStatus(ctx context.Context, command admin.SetAdminAccountStatusCommand) (admin.AdminAccountView, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return admin.AdminAccountView{}, err
	}
	defer func() { _ = tx.Rollback() }()

	var currentRole, currentStatus, username string
	var currentVersion int64
	err = tx.QueryRowContext(ctx, `SELECT username, role, status, version FROM admin_accounts WHERE id = $1 FOR UPDATE`, command.AccountID).
		Scan(&username, &currentRole, &currentStatus, &currentVersion)
	if errors.Is(err, sql.ErrNoRows) {
		return admin.AdminAccountView{}, admin.ErrAdminAccountNotFound
	}
	if err != nil {
		return admin.AdminAccountView{}, err
	}
	if currentVersion != command.Version {
		return admin.AdminAccountView{}, admin.ErrAdminAccountVersionConflict
	}
	targetStatus := "ACTIVE"
	if command.Status == admin.AdminAccountDisabled {
		targetStatus = "DISABLED"
		if currentRole == "SUPER_ADMIN" {
			var activeSuperAdmins int64
			if err := tx.QueryRowContext(ctx, `SELECT count(*) FROM admin_accounts WHERE role = 'SUPER_ADMIN' AND status = 'ACTIVE'`).Scan(&activeSuperAdmins); err != nil {
				return admin.AdminAccountView{}, err
			}
			if activeSuperAdmins <= 1 {
				return admin.AdminAccountView{}, admin.ErrLastSuperAdmin
			}
		}
	}

	var view admin.AdminAccountView
	var role string
	err = tx.QueryRowContext(ctx, `UPDATE admin_accounts
SET status = $2, version = version + 1, updated_at = CURRENT_TIMESTAMP
WHERE id = $1
RETURNING id, username, role, CASE status WHEN 'ACTIVE' THEN 1 ELSE 0 END,
          must_change_password, version`, command.AccountID, targetStatus).
		Scan(&view.ID, &view.Username, &role, &view.Status, &view.MustChangePassword, &view.Version)
	if err != nil {
		return admin.AdminAccountView{}, err
	}
	view.Roles = []string{role}
	payload, _ := json.Marshal(map[string]any{"status": targetStatus, "reason": command.Reason})
	if _, err := tx.ExecContext(ctx, `INSERT INTO operation_logs
    (actor_type, actor_id, action, resource_type, resource_id, request_id, payload)
VALUES ('ADMIN', $1, 'admin_account.status', 'admin_account', $2, $3, $4::jsonb)`,
		fmt.Sprintf("%d", command.ActorID), fmt.Sprintf("%d", command.AccountID), command.RequestID, string(payload)); err != nil {
		return admin.AdminAccountView{}, err
	}
	if err := tx.Commit(); err != nil {
		return admin.AdminAccountView{}, err
	}
	return view, nil
}

var _ interface {
	ListAdminAccounts(context.Context, int64, int64) (admin.AdminAccountPage, error)
	CreateAdminAccount(context.Context, admin.CreateAdminAccountCommand) (admin.AdminAccountView, error)
	SetAdminAccountStatus(context.Context, admin.SetAdminAccountStatusCommand) (admin.AdminAccountView, error)
} = (*AdminStore)(nil)
