package auth

import (
	"context"
	"errors"
	"fmt"
	"regexp"
	"strings"
	"time"
	"unicode/utf8"
)

// Profile scope errors. They map onto the shared error-code registry in the
// HTTP layer: invalid nickname → 400, unknown account → 404, frozen → 403.
var (
	// ErrInvalidNickname reports a nickname outside UC-U-05 bounds (1..20
	// characters, not pure whitespace).
	ErrInvalidNickname = errors.New("auth: nickname length is outside 1..20 or pure whitespace")
	// ErrProfileNotFound reports a missing account (deleted accounts are
	// anonymized and disappear from lookups).
	ErrProfileNotFound = errors.New("auth: account not found")
	// ErrAccountDeleted reports an account that has already been deleted.
	ErrAccountDeleted = errors.New("auth: account is deleted")
	// ErrInvalidAdminActor reports a missing administrator actor for a
	// management operation.
	ErrInvalidAdminActor = errors.New("auth: administrator actor is required")
)

var nicknamePattern = regexp.MustCompile(`^\S(.*\S)?$`)

// IsValidNickname applies UC-U-05: 1..20 characters, not pure whitespace.
func IsValidNickname(nickname string) bool {
	length := utf8.RuneCountInString(nickname)
	if length < 1 || length > 20 {
		return false
	}
	return nicknamePattern.MatchString(nickname)
}

// MaskPhone applies the UC-U-05 display rule: the middle four digits of the
// phone are masked (138****5678). Non-standard inputs are fully masked.
func MaskPhone(phone string) string {
	if len(phone) != 11 {
		return "***"
	}
	return phone[:3] + "****" + phone[7:]
}

// ProfileView is the user-center payload (UC-U-05): masked phone, nickname,
// avatar and registration time. The wallet balance belongs to the wallet
// domain and is intentionally not part of the identity profile.
type ProfileView struct {
	ID           int64     `json:"id"`
	PhoneMasked  string    `json:"phoneMasked"`
	DisplayName  string    `json:"displayName"`
	AvatarURL    string    `json:"avatarUrl,omitempty"`
	Status       string    `json:"status"`
	RegisteredAt time.Time `json:"registeredAt"`
}

// ProfileUpdate carries a validated profile change. A nil field means "not
// changed in this request".
type ProfileUpdate struct {
	DisplayName *string
	AvatarURL   *string
}

// AccountMutation is the persistence port for profile state changes. The
// B line (B-02) owns the PostgreSQL implementation; A-01 ships the contract
// plus an in-memory test double. Every method must run inside a transaction
// on the implementation side.
//
// Deletion follows UC-U-05 申请注销: the account is anonymized in place
// (phone and display name, password dropped, status DISABLED) while the
// business records stay; the returned flag reports whether the account was
// still present. Freezing sets status DISABLED without anonymizing.
type AccountMutation interface {
	GetProfile(ctx context.Context, userID int64) (ProfileView, error)
	UpdateProfile(ctx context.Context, userID int64, update ProfileUpdate) (ProfileView, error)
	DeleteAccount(ctx context.Context, userID int64) (bool, error)
	SetFrozen(ctx context.Context, userID int64, frozen bool) (bool, error)
}

// FreezeUser disables an account (BR-07): the user can no longer log in or
// start charging, and every live session is revoked immediately.
// InMemoryAccountMutation is the exported test double for the profile
// mutation port. Cross-package tests (repository integration) construct it
// directly, mirroring InMemorySessionStore and InMemorySMSCodeStore.
type InMemoryAccountMutation struct {
	accounts map[int64]*UserAccount
}

// NewInMemoryAccountMutation returns a mutation double over the given
// accounts map (shared pointer semantics).
func NewInMemoryAccountMutation(accounts map[int64]*UserAccount) *InMemoryAccountMutation {
	return &InMemoryAccountMutation{accounts: accounts}
}

// GetProfile returns the profile view.
func (s *InMemoryAccountMutation) GetProfile(_ context.Context, userID int64) (ProfileView, error) {
	account, ok := s.accounts[userID]
	if !ok {
		return ProfileView{}, ErrProfileNotFound
	}
	return ProfileView{ID: account.ID, PhoneMasked: account.Phone, DisplayName: account.DisplayName, Status: account.Status, RegisteredAt: time.Now()}, nil
}

// UpdateProfile applies nickname and/or avatar changes.
func (s *InMemoryAccountMutation) UpdateProfile(_ context.Context, userID int64, update ProfileUpdate) (ProfileView, error) {
	account, ok := s.accounts[userID]
	if !ok {
		return ProfileView{}, ErrProfileNotFound
	}
	if update.DisplayName != nil {
		account.DisplayName = *update.DisplayName
	}
	if update.AvatarURL != nil {
		account.Email = *update.AvatarURL
	}
	return ProfileView{ID: account.ID, PhoneMasked: account.Phone, DisplayName: account.DisplayName, Status: account.Status}, nil
}

// DeleteAccount removes the account.
func (s *InMemoryAccountMutation) DeleteAccount(_ context.Context, userID int64) (bool, error) {
	if _, ok := s.accounts[userID]; !ok {
		return false, nil
	}
	delete(s.accounts, userID)
	return true, nil
}

// SetFrozen toggles the account status.
func (s *InMemoryAccountMutation) SetFrozen(_ context.Context, userID int64, frozen bool) (bool, error) {
	account, ok := s.accounts[userID]
	if !ok {
		return false, nil
	}
	if frozen {
		account.Status = StatusDisable
	} else {
		account.Status = StatusActive
	}
	return true, nil
}

var _ AccountMutation = (*InMemoryAccountMutation)(nil)

func (s *Service) FreezeUser(ctx context.Context, adminID, userID int64) error {
	if adminID < 1 {
		return ErrInvalidAdminActor
	}
	found, err := s.mutations.SetFrozen(ctx, userID, true)
	if err != nil {
		return fmt.Errorf("auth: freeze account: %w", err)
	}
	if !found {
		return ErrProfileNotFound
	}
	return s.sessions.RevokeAllForUser(ctx, userID)
}

// UnfreezeUser re-enables a frozen account.
func (s *Service) UnfreezeUser(ctx context.Context, adminID, userID int64) error {
	if adminID < 1 {
		return ErrInvalidAdminActor
	}
	profile, err := s.mutations.GetProfile(ctx, userID)
	if err != nil {
		return fmt.Errorf("auth: inspect account before unfreeze: %w", err)
	}
	if profile.Status == StatusActive {
		return nil
	}
	// Revoke before enabling login. If Redis is unavailable, leaving the
	// account disabled prevents sessions missed by an earlier freeze attempt
	// from becoming valid again.
	if err := s.sessions.RevokeAllForUser(ctx, userID); err != nil {
		return fmt.Errorf("auth: revoke sessions before unfreeze: %w", err)
	}
	found, err := s.mutations.SetFrozen(ctx, userID, false)
	if err != nil {
		return fmt.Errorf("auth: unfreeze account: %w", err)
	}
	if !found {
		return ErrProfileNotFound
	}
	return nil
}

// GetProfile returns the caller's own profile view.
func (s *Service) GetProfile(ctx context.Context, userID int64) (ProfileView, error) {
	if userID < 1 {
		return ProfileView{}, ErrProfileNotFound
	}
	view, err := s.mutations.GetProfile(ctx, userID)
	if err != nil {
		return ProfileView{}, err
	}
	view.PhoneMasked = MaskPhone(view.PhoneMasked)
	return view, nil
}

// UpdateProfile applies a nickname and/or avatar change to the caller's own
// account. An empty avatar URL string clears the avatar.
func (s *Service) UpdateProfile(ctx context.Context, userID int64, update ProfileUpdate) (ProfileView, error) {
	if userID < 1 {
		return ProfileView{}, ErrProfileNotFound
	}
	if update.DisplayName != nil {
		nickname := strings.TrimSpace(*update.DisplayName)
		if !IsValidNickname(nickname) {
			return ProfileView{}, ErrInvalidNickname
		}
		update.DisplayName = &nickname
	}
	if update.AvatarURL != nil && strings.TrimSpace(*update.AvatarURL) == "" {
		cleared := ""
		update.AvatarURL = &cleared
	}
	view, err := s.mutations.UpdateProfile(ctx, userID, update)
	if err != nil {
		return ProfileView{}, err
	}
	view.PhoneMasked = MaskPhone(view.PhoneMasked)
	return view, nil
}

// DeleteAccount anonymizes the caller's account and revokes every live
// session (UC-U-05 申请注销). The returned flag reports whether the account
// existed; the current token is revoked by the handler afterwards.
func (s *Service) DeleteAccount(ctx context.Context, userID int64, token string) (bool, error) {
	if userID < 1 {
		return false, ErrProfileNotFound
	}
	existed, err := s.mutations.DeleteAccount(ctx, userID)
	if err != nil {
		return false, err
	}
	if !existed {
		return false, ErrProfileNotFound
	}
	if err := s.sessions.RevokeAllForUser(ctx, userID); err != nil {
		return existed, err
	}
	return true, nil
}
