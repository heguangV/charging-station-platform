package admin

import (
	"context"
	"errors"
	"regexp"
	"strings"
	"unicode/utf8"

	"github.com/heguangV/charging-station-platform/backend/internal/auth"
)

const (
	AdminAccountEnabled  = 1
	AdminAccountDisabled = 0
)

var (
	ErrAdminAccountManagementUnavailable = errors.New("admin: account management store is unavailable")
	ErrInvalidAdminAccount               = errors.New("admin: invalid administrator account")
	ErrAdminAccountNotFound              = errors.New("admin: administrator account not found")
	ErrAdminAccountExists                = errors.New("admin: administrator username already exists")
	ErrAdminAccountVersionConflict       = errors.New("admin: administrator account version conflict")
	ErrAdminAccountSelfDisable           = errors.New("admin: administrator cannot disable the current account")
	ErrLastSuperAdmin                    = errors.New("admin: the last active super administrator cannot be disabled")
)

var adminUsernamePattern = regexp.MustCompile(`^[A-Za-z0-9_]{3,32}$`)

type AdminAccountView struct {
	ID                 int64    `json:"id"`
	Username           string   `json:"username"`
	Roles              []string `json:"roles"`
	Status             int      `json:"status"`
	MustChangePassword bool     `json:"mustChangePassword"`
	Version            int64    `json:"version"`
}

type AdminAccountPage struct {
	Items []AdminAccountView `json:"items"`
	Meta  PageMeta           `json:"meta"`
}

type CreateAdminAccountCommand struct {
	ActorID      int64
	Username     string
	PasswordHash string
	Reason       string
	RequestID    string
}

type SetAdminAccountStatusCommand struct {
	ActorID   int64
	AccountID int64
	Status    int
	Version   int64
	Reason    string
	RequestID string
}

type accountManagementStore interface {
	ListAdminAccounts(ctx context.Context, page, pageSize int64) (AdminAccountPage, error)
	CreateAdminAccount(ctx context.Context, command CreateAdminAccountCommand) (AdminAccountView, error)
	SetAdminAccountStatus(ctx context.Context, command SetAdminAccountStatusCommand) (AdminAccountView, error)
}

func (s *Service) Accounts(ctx context.Context, page, pageSize int64) (AdminAccountPage, error) {
	if page < 1 || pageSize < 1 || pageSize > 100 {
		return AdminAccountPage{}, ErrInvalidAdminAccount
	}
	if s.accounts == nil {
		return AdminAccountPage{}, ErrAdminAccountManagementUnavailable
	}
	return s.accounts.ListAdminAccounts(ctx, page, pageSize)
}

func (s *Service) CreateAccount(ctx context.Context, actorID int64, username, password, reason, requestID string) (AdminAccountView, error) {
	username = strings.TrimSpace(username)
	reason = strings.TrimSpace(reason)
	if actorID < 1 || !adminUsernamePattern.MatchString(username) || len(password) < 10 || len(password) > 128 || !validAdminReason(reason) {
		return AdminAccountView{}, ErrInvalidAdminAccount
	}
	if s.accounts == nil {
		return AdminAccountView{}, ErrAdminAccountManagementUnavailable
	}
	passwordHash, err := auth.HashPassword(password)
	if err != nil {
		return AdminAccountView{}, ErrInvalidAdminAccount
	}
	return s.accounts.CreateAdminAccount(ctx, CreateAdminAccountCommand{
		ActorID: actorID, Username: username, PasswordHash: passwordHash, Reason: reason, RequestID: requestID,
	})
}

func (s *Service) SetAccountStatus(ctx context.Context, command SetAdminAccountStatusCommand) (AdminAccountView, error) {
	command.Reason = strings.TrimSpace(command.Reason)
	if command.ActorID < 1 || command.AccountID < 1 || command.Version < 1 ||
		(command.Status != AdminAccountEnabled && command.Status != AdminAccountDisabled) || !validAdminReason(command.Reason) {
		return AdminAccountView{}, ErrInvalidAdminAccount
	}
	if command.ActorID == command.AccountID && command.Status == AdminAccountDisabled {
		return AdminAccountView{}, ErrAdminAccountSelfDisable
	}
	if s.accounts == nil {
		return AdminAccountView{}, ErrAdminAccountManagementUnavailable
	}
	return s.accounts.SetAdminAccountStatus(ctx, command)
}

func validAdminReason(reason string) bool {
	length := utf8.RuneCountInString(reason)
	return length >= 2 && length <= 200
}
