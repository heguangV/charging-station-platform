package admin

import (
	"context"
	"errors"
	"fmt"
	"regexp"
	"strings"
	"unicode"
	"unicode/utf8"
)

// Manual user archiving.
//
// Most accounts arrive by themselves: a phone that signs in with an SMS code
// registers automatically (UC-U-01). What that loop cannot produce is the
// accounts an operator enrolls on someone's behalf - a fleet of house
// accounts, a roster migrated from another platform, a demo dataset. This is
// that operation, for one account or for a page of a roster at a time.
//
// An archived account is born the way a self-registered one is: ACTIVE, with
// a zero-balance wallet and no password. The user keeps signing in through
// SMS, which is why no credential is accepted here - a password an operator
// chose for a user is a credential the user never agreed to carry.
//
// Phones are the account's identity on this platform, and every entry names
// one. A duplicate is therefore a conflict (409), not a warning to skip:
// skipping would leave the operator guessing which entries of the roster
// landed and which did not.

const (
	// MaxUserBatch bounds one roster page. Beyond this the seed tooling is the
	// right instrument, not an HTTP request.
	MaxUserBatch = 1000
	// maxDisplayNameRunes matches the profile contract's own bound for a
	// display name; an archive entry becomes the same field.
	maxDisplayNameRunes = 20
)

// User archive errors.
var (
	// ErrInvalidUserDraft reports a draft the platform will not store: a phone
	// outside the format every other phone on the platform uses, or a display
	// name outside its contract bound.
	ErrInvalidUserDraft = errors.New("admin: invalid user draft")
	// ErrInvalidUserBatch reports a batch that is empty or over the roster-page
	// bound.
	ErrInvalidUserBatch = errors.New("admin: invalid user batch")
	// ErrDuplicateUserPhone reports a phone that is already registered. It maps
	// to 409 ALREADY_EXISTS: the request is well formed, it just collides with
	// an account that exists.
	ErrDuplicateUserPhone = errors.New("admin: duplicate user phone")
)

// phonePattern is the same shape the SMS login accepts: an archived account
// and a self-registered one must be interchangeable at the door.
var phonePattern = regexp.MustCompile(`^1[3-9]\d{9}$`)

// UserDraft is one account to archive.
//
// It carries only what an operator types in: the phone that will own the
// account and, optionally, the name it goes by. Status, balance and
// credentials are the platform's to decide.
type UserDraft struct {
	Phone string
	// DisplayName is optional. An empty draft takes the registration default:
	// 用户 plus the last four phone digits, exactly as UC-U-01 names a
	// self-registered account.
	DisplayName string
}

// CreateUserCommand carries a validated single-account request.
type CreateUserCommand struct {
	AdminID        int64
	User           UserDraft
	IdempotencyKey string
	RequestHash    string
	TraceID        string
}

// CreateUsersCommand carries a validated roster-page request.
type CreateUsersCommand struct {
	AdminID        int64
	Users          []UserDraft
	IdempotencyKey string
	RequestHash    string
	TraceID        string
}

// UserRecord is one archived account as the console receives it.
type UserRecord struct {
	ID          int64  `json:"id"`
	Phone       string `json:"phone"`
	DisplayName string `json:"displayName"`
	Status      string `json:"status"`
	// BalanceCent is always zero at creation. It is stated so the console can
	// render an archived account with the same card it uses everywhere else.
	BalanceCent int64 `json:"balanceCent"`
}

// UserBatchResult is what a roster page reports back.
type UserBatchResult struct {
	// UserCount is how many accounts this request created. It equals
	// len(Created) and is stated separately so a client that only needs the
	// count does not have to walk the list.
	UserCount int64        `json:"userCount"`
	Created   []UserRecord `json:"created"`
}

// CreateUser archives one account.
//
// Everything is checked before the store is called, so a request that cannot
// succeed never opens a transaction.
func (s *Service) CreateUser(ctx context.Context, command CreateUserCommand) (UserRecord, error) {
	if command.AdminID < 1 {
		return UserRecord{}, ErrInvalidAdminActor
	}
	draft, err := normalizeUserDraft(command.User)
	if err != nil {
		return UserRecord{}, err
	}
	command.User = draft
	return s.store.CreateUser(ctx, command)
}

// CreateUsers archives a page of a roster.
//
// Phones must be unique inside the page as well as against the platform: a
// repeated phone is refused here, naming the phone, instead of leaving the
// caller to compare two rows by eye.
func (s *Service) CreateUsers(ctx context.Context, command CreateUsersCommand) (UserBatchResult, error) {
	if command.AdminID < 1 {
		return UserBatchResult{}, ErrInvalidAdminActor
	}
	if len(command.Users) < 1 || len(command.Users) > MaxUserBatch {
		return UserBatchResult{}, fmt.Errorf("%w: a batch carries 1 to %d accounts", ErrInvalidUserBatch, MaxUserBatch)
	}

	seen := make(map[string]bool, len(command.Users))
	normalized := make([]UserDraft, 0, len(command.Users))
	for index := range command.Users {
		draft, err := normalizeUserDraft(command.Users[index])
		if err != nil {
			return UserBatchResult{}, fmt.Errorf("%w: entry %d", err, index+1)
		}
		if seen[draft.Phone] {
			return UserBatchResult{}, fmt.Errorf("%w: %s appears twice in the request", ErrDuplicateUserPhone, draft.Phone)
		}
		seen[draft.Phone] = true
		normalized = append(normalized, draft)
	}
	command.Users = normalized
	return s.store.CreateUsers(ctx, command)
}

// normalizeUserDraft trims and validates one draft. The checks are the ones
// every other entry door on the platform applies: the phone shape of the SMS
// login, and the display-name bound of the profile contract. A name carrying
// a control character is refused for the same reason a charger code is: it
// would be invisible in a user list.
func normalizeUserDraft(draft UserDraft) (UserDraft, error) {
	draft.Phone = strings.TrimSpace(draft.Phone)
	draft.DisplayName = strings.TrimSpace(draft.DisplayName)
	if !phonePattern.MatchString(draft.Phone) {
		return UserDraft{}, fmt.Errorf("%w: %q is not a phone this platform signs in", ErrInvalidUserDraft, draft.Phone)
	}
	if utf8.RuneCountInString(draft.DisplayName) > maxDisplayNameRunes {
		return UserDraft{}, fmt.Errorf("%w: display name is longer than %d runes", ErrInvalidUserDraft, maxDisplayNameRunes)
	}
	for _, character := range draft.DisplayName {
		if unicode.IsControl(character) {
			return UserDraft{}, fmt.Errorf("%w: display name carries a control character", ErrInvalidUserDraft)
		}
	}
	return draft, nil
}

// DefaultUserDisplayName is the registration default, mirrored so an archived
// account without a name is indistinguishable from a self-registered one.
func DefaultUserDisplayName(phone string) string {
	if len(phone) < 4 {
		return "用户"
	}
	return "用户" + phone[len(phone)-4:]
}

// MaskPhone hides the middle four digits. The audit trail is read by more
// eyes than the user table is, and the security baseline keeps full phone
// numbers out of logs; a masked phone still answers "which entry was this"
// to the operator who typed it.
func MaskPhone(phone string) string {
	if len(phone) < 8 {
		return "***"
	}
	return phone[:3] + "****" + phone[len(phone)-4:]
}
