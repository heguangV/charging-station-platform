// Package review implements the A-05 domain: post-charge order reviews, the
// per-station comment wall, order appeals and their admin approval.
//
// Requirement baseline: UC-U-12 (订单评价与评论墙) and UC-U-09 (申诉与审核),
// BR-06. One review and at most one appeal per order; identical-content
// replays return the first result, different content conflicts; the author
// shown on the wall is the nickname, the masked phone or 已注销用户 — never
// the full phone, user id or order number.
package review

import (
	"context"
	"errors"
	"strings"
	"time"
	"unicode/utf8"
)

// Review and appeal states.
const (
	AppealPending  = "PENDING"
	AppealApproved = "APPROVED"
	// AppealRejected dismisses the appeal: the order and the wallet stay as they are.
	AppealRejected = "REJECTED"

	AuthorDeleted = "已注销用户"
)

// Comment bounds from UC-U-12: 1..5 stars, 1..500 Unicode code points after
// trimming. Appeal reason: 1..500 code points after trimming (UC-U-09).
const (
	MinStars         = 1
	MaxStars         = 5
	MaxCommentLength = 500
	MinReasonLength  = 1
)

var (
	// ErrInvalidStars maps to 400 VALIDATION_FAILED semantics.
	ErrInvalidStars = errors.New("review: stars must be 1..5")
	// ErrInvalidComment reports an empty or oversized comment.
	ErrInvalidComment = errors.New("review: comment length is outside 1..500 code points")
	// ErrInvalidReason reports an empty or oversized appeal reason.
	ErrInvalidReason = errors.New("review: appeal reason length is outside 1..500 code points")
	// ErrOrderNotReviewable maps to 409: only the owner's COMPLETED order
	// without an appeal can be reviewed.
	ErrOrderNotReviewable = errors.New("review: order is not reviewable")
	// ErrOrderNotAppealable maps to 409: only the owner's COMPLETED order
	// without a pending or approved appeal can be appealed.
	ErrOrderNotAppealable = errors.New("review: order is not appealable")
	// ErrReviewConflict maps to 409 ALREADY_EXISTS: same order, different
	// content.
	ErrReviewConflict = errors.New("review: review already exists with different content")
	// ErrAppealConflict maps to 409: a different-content appeal already
	// exists for the order.
	ErrAppealConflict = errors.New("review: appeal already exists with different content")
	// ErrAppealAlreadyApproved maps to 409. A repeated admin decision is a
	// no-op success, so the service returns this only for a malformed admin
	// identity.
	ErrAppealAlreadyApproved = errors.New("review: appeal is already approved")
	// ErrNotFound maps to 404 for a missing review or appeal.
	ErrNotFound = errors.New("review: not found")
	// ErrNotOrderOwner maps to 404 like the order domain: cross-user access
	// is indistinguishable from a missing record.
	ErrNotOrderOwner = errors.New("review: not the order owner")
)

// TrimComment applies the trimming rule and validates the code-point count.
func TrimComment(comment string) (string, error) {
	trimmed := strings.TrimSpace(comment)
	if strings.ContainsRune(trimmed, '\x00') || utf8.RuneCountInString(trimmed) < 1 || utf8.RuneCountInString(trimmed) > MaxCommentLength {
		return "", ErrInvalidComment
	}
	return trimmed, nil
}

// TrimReason applies the appeal trimming rule.
func TrimReason(reason string) (string, error) {
	trimmed := strings.TrimSpace(reason)
	if strings.ContainsRune(trimmed, '\x00') || utf8.RuneCountInString(trimmed) < MinReasonLength || utf8.RuneCountInString(trimmed) > MaxCommentLength {
		return "", ErrInvalidReason
	}
	return trimmed, nil
}

// ReviewView is the stored review as returned to its author.
type ReviewView struct {
	OrderNo   string    `json:"orderNo"`
	Stars     int       `json:"stars"`
	Comment   string    `json:"comment"`
	CreatedAt time.Time `json:"createdAt"`
}

// WallEntry is one comment-wall item: no user id, no order number, author
// masked per UC-U-12.
type WallEntry struct {
	Stars     int       `json:"stars"`
	Comment   string    `json:"comment"`
	Author    string    `json:"author"`
	CreatedAt time.Time `json:"createdAt"`
}

// WallPage is one page of the station comment wall.
type WallPage struct {
	Items []WallEntry `json:"items"`
	Meta  PageMeta    `json:"meta"`
}

// PageMeta is the contract pagination metadata.
type PageMeta struct {
	Page     int64 `json:"page"`
	PageSize int64 `json:"pageSize"`
	Total    int64 `json:"total"`
}

// AppealView is the stored appeal as returned to its author and admins.
type AppealView struct {
	ID              int64      `json:"id"`
	OrderNo         string     `json:"orderNo"`
	Reason          string     `json:"reason"`
	Status          string     `json:"status"`
	CreatedAt       time.Time  `json:"createdAt"`
	DecidedAt       *time.Time `json:"decidedAt,omitempty"`
	OrderAmountCent int64      `json:"orderAmountCent"`
	OrderPaidCent   int64      `json:"orderPaidCent"`
	// DecisionReason is the operator's reason, present when the appeal was rejected.
	DecisionReason *string `json:"decisionReason,omitempty"`
}

// AppealPage is one page of appeals (admin queue).
type AppealPage struct {
	Items []AppealView `json:"items"`
	Meta  PageMeta     `json:"meta"`
}

// WallFilter carries validated comment-wall parameters.
type WallFilter struct {
	StationID int64
	Page      int64
	PageSize  int64
}

// AppealFilter carries the admin queue parameters.
type AppealFilter struct {
	Status   string // "", PENDING, APPROVED or REJECTED
	Page     int64
	PageSize int64
}

// Store is the persistence port; the B line (B-02) owns the PostgreSQL
// implementation. Mutating methods run in one transaction covering the
// review/appeal rows and any order or wallet state they imply.
type Store interface {
	// CreateReview inserts the review for a COMPLETED order owned by the
	// user. Identical-content replays return the stored review; different
	// content returns ErrReviewConflict. Non-reviewable orders return
	// ErrOrderNotReviewable.
	CreateReview(ctx context.Context, userID int64, orderNo string, stars int, comment string) (ReviewView, error)
	// GetReview returns the user's own review for the order.
	GetReview(ctx context.Context, userID int64, orderNo string) (ReviewView, error)
	// ListWall returns one page of the station comment wall with authors
	// already masked.
	ListWall(ctx context.Context, filter WallFilter) (WallPage, error)
	// CreateAppeal records the appeal for a COMPLETED order owned by the
	// user. Same-content replays return the stored appeal; different
	// content returns ErrAppealConflict.
	CreateAppeal(ctx context.Context, userID int64, orderNo string, reason string) (AppealView, error)
	// ListAppeals returns one page of appeals for the admin queue.
	ListAppeals(ctx context.Context, filter AppealFilter) (AppealPage, error)
	// GetAppeal returns one appeal by id.
	GetAppeal(ctx context.Context, appealID int64) (AppealView, error)
	// GetAppealByOrder returns the caller's own appeal for the order.
	GetAppealByOrder(ctx context.Context, userID int64, orderNo string) (AppealView, error)
	// ApproveAppeal marks the appeal approved, cancels the order and
	// refunds the settled amount to the user's wallet — all in the same
	// transaction. It returns false when the appeal was already approved;
	// the service treats that repeated decision as a no-op success.
	ApproveAppeal(ctx context.Context, appealID int64, adminID int64) (bool, error)
	// RejectAppeal dismisses the appeal: it becomes REJECTED with the
	// operator's reason, while the order and the wallet stay untouched.
	// It returns false when the appeal already carries a decision, which the
	// service treats as a no-op success (repeated decisions never re-act).
	RejectAppeal(ctx context.Context, appealID int64, adminID int64, reason string) (bool, error)
}

// Service validates commands and delegates persistence to a Store.
type Service struct {
	store Store
	clock func() time.Time
}

// NewService wires the service to its store.
func NewService(store Store) (*Service, error) {
	if store == nil {
		return nil, errors.New("review: store is required")
	}
	return &Service{store: store, clock: time.Now}, nil
}

// Create validates and creates the review for the caller's order.
func (s *Service) Create(ctx context.Context, userID int64, orderNo string, stars int, comment string) (ReviewView, error) {
	if orderNo == "" {
		return ReviewView{}, ErrOrderNotReviewable
	}
	if stars < MinStars || stars > MaxStars {
		return ReviewView{}, ErrInvalidStars
	}
	trimmed, err := TrimComment(comment)
	if err != nil {
		return ReviewView{}, err
	}
	return s.store.CreateReview(ctx, userID, orderNo, stars, trimmed)
}

// Get returns the caller's own review.
func (s *Service) Get(ctx context.Context, userID int64, orderNo string) (ReviewView, error) {
	if orderNo == "" {
		return ReviewView{}, ErrNotFound
	}
	return s.store.GetReview(ctx, userID, orderNo)
}

// Wall returns one page of the station comment wall.
func (s *Service) Wall(ctx context.Context, filter WallFilter) (WallPage, error) {
	if filter.StationID < 1 {
		return WallPage{}, ErrNotFound
	}
	if filter.Page < 1 || filter.PageSize < 1 || filter.PageSize > 100 {
		return WallPage{}, ErrInvalidComment
	}
	return s.store.ListWall(ctx, filter)
}

// CreateAppeal validates and records the caller's appeal.
func (s *Service) CreateAppeal(ctx context.Context, userID int64, orderNo string, reason string) (AppealView, error) {
	if orderNo == "" {
		return AppealView{}, ErrOrderNotAppealable
	}
	trimmed, err := TrimReason(reason)
	if err != nil {
		return AppealView{}, err
	}
	view, err := s.store.CreateAppeal(ctx, userID, orderNo, trimmed)
	if err != nil {
		return AppealView{}, err
	}
	return view, nil
}

// GetAppeal returns the caller's own appeal for one order.
func (s *Service) GetAppeal(ctx context.Context, userID int64, orderNo string) (AppealView, error) {
	if orderNo == "" {
		return AppealView{}, ErrNotFound
	}
	return s.store.GetAppealByOrder(ctx, userID, orderNo)
}

// Appeals returns one page of the admin appeal queue.
func (s *Service) Appeals(ctx context.Context, filter AppealFilter) (AppealPage, error) {
	if filter.Page < 1 || filter.PageSize < 1 || filter.PageSize > 100 {
		return AppealPage{}, ErrInvalidReason
	}
	switch filter.Status {
	case "", AppealPending, AppealApproved, AppealRejected:
	default:
		return AppealPage{}, ErrInvalidReason
	}
	return s.store.ListAppeals(ctx, filter)
}

// Approve applies the admin decision (UC-U-09 pt 5): the appeal is marked
// approved, the order is cancelled and the settled amount refunded. A
// repeated decision is a no-op that succeeds with the current result
// (UC-U-09: 相同申诉和重复审核不得重复记账).
func (s *Service) Approve(ctx context.Context, appealID int64, adminID int64) error {
	if adminID < 1 {
		return ErrAppealAlreadyApproved
	}
	_, err := s.store.ApproveAppeal(ctx, appealID, adminID)
	return err
}

// Reject dismisses an appeal: the operator's reason is audited and shown to the
// customer. Repeating the decision is a no-op that succeeds - an appeal that
// already carries a decision is never re-acted on.
func (s *Service) Reject(ctx context.Context, appealID int64, adminID int64, reason string) error {
	if adminID < 1 {
		return ErrAppealAlreadyApproved
	}
	trimmed, err := TrimReason(reason)
	if err != nil {
		return err
	}
	_, err = s.store.RejectAppeal(ctx, appealID, adminID, trimmed)
	return err
}

var _ = strings.TrimSpace
