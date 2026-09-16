// Package admin implements the P0 management API: station management,
// charger management, user and order queries, device commands and the
// operation audit trail.
//
// Authorization is layered in the middleware (auth.RequireAdminWrite):
// every admin role may read, only SUPER_ADMIN and OPERATOR may write.
package admin

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"fmt"
	"regexp"
	"strings"
	"time"

	"github.com/heguangV/charging-station-platform/backend/internal/order"
	"github.com/heguangV/charging-station-platform/backend/internal/station"
	"github.com/heguangV/charging-station-platform/backend/internal/wallet"
)

// Charger and user status values from the frozen contract.
const (
	ChargerStatusIdle       = "IDLE"
	ChargerStatusFault      = "FAULT"
	ChargerStatusRestarting = "RESTARTING"
	ChargerStatusOccupied   = "OCCUPIED"
	ChargerStatusDisabled   = "DISABLED"

	UserStatusActive   = "ACTIVE"
	UserStatusDisabled = "DISABLED"

	CommandPending = "PENDING"
)

// Status filter values for the user list query parameter (integer 0/1 in
// the contract): 1 maps to ACTIVE, 0 to DISABLED.
const (
	UserStatusQueryActive   = 1
	UserStatusQueryDisabled = 0
)

// Sentinel errors mapped by the HTTP layer to the shared error-code registry.
var (
	// ErrInvalidStationFilter reports a malformed admin list request.
	ErrInvalidStationFilter = errors.New("admin: invalid station filter")
	// ErrInvalidAdminActor reports a missing or non-writer administrator.
	ErrInvalidAdminActor = errors.New("admin: administrator may not perform this operation")
	// ErrInvalidReason reports a restart reason outside the contract bounds.
	ErrInvalidReason = errors.New("admin: restart reason length is outside 2..200")
	// ErrInvalidTariff reports a malformed tariff or target-status value.
	ErrInvalidTariff = errors.New("admin: invalid tariff or target status")
	// ErrInvalidLedgerFilter reports a malformed ledger or audit query.
	ErrInvalidLedgerFilter = errors.New("admin: invalid ledger or audit filter")
	// ErrUserNotFound maps to 404 NOT_FOUND for a missing user.
	ErrUserNotFound = errors.New("admin: user not found")
	// ErrInvalidUserStatus reports a status query value outside 0/1.
	ErrInvalidUserStatus = errors.New("admin: invalid user status")
	// ErrChargerUnavailable maps to 409 CHARGER_UNAVAILABLE: the charger is
	// occupied, disabled or unknown.
	ErrChargerUnavailable = errors.New("admin: charger is unavailable")
	// ErrInvalidStateTransition maps to 409 INVALID_STATE_TRANSITION: the
	// charger is already restarting.
	ErrInvalidStateTransition = errors.New("admin: charger is already restarting")
)

var stationCodePattern = regexp.MustCompile(`^[A-Za-z0-9_-]{2,32}$`)

// UserSummary is the contract UserSummary payload.
type UserSummary struct {
	ID          int64  `json:"id"`
	DisplayName string `json:"displayName"`
	Status      string `json:"status"`
	BalanceCent int64  `json:"balanceCent"`
}

// UserPage is one page of users.
type UserPage struct {
	Items []UserSummary `json:"items"`
	Meta  PageMeta      `json:"meta"`
}

// StationPage and ChargerPage reuse the user-side payloads; the admin views
// additionally see DISABLED stations.
type StationPage = station.StationPage
type ChargerPage = station.ChargerPage
type OrderPage = order.OrderPage

// PageMeta is the contract pagination metadata.
type PageMeta struct {
	Page     int64 `json:"page"`
	PageSize int64 `json:"pageSize"`
	Total    int64 `json:"total"`
}

// UserFilter carries validated user list parameters.
type UserFilter struct {
	Page     int64
	PageSize int64
	Keyword  string
	Status   string // "", ACTIVE or DISABLED
}

// AdminOrderFilter carries validated admin order list parameters.
type AdminOrderFilter struct {
	Page     int64
	PageSize int64
	OrderNo  string
	Status   string
	// UserID narrows the list to one user's orders (the management UI opens a
	// user's detail page and asks for that user's orders). Zero means "no user
	// filter": user ids start at 1, the same convention OrderNo uses for "".
	UserID int64
}

// Command is the contract Command payload returned when a device command is
// accepted (202).
//
// The identifier is commandId, the same name the query endpoint uses and the
// same value that lives in charger_command_outcomes.command_id. It used to be
// called commandNo here while the column and every other layer said command_id;
// two names for one identifier is how a "no such command" bug hides.
type Command struct {
	CommandID string `json:"commandId"`
	Status    string `json:"status"`
}

// DeviceCommand is what the platform recorded about one device command: the
// outcome the gateway reported and whether it has been applied to the order.
type DeviceCommand struct {
	CommandID  string `json:"commandId"`
	ChargerID  int64  `json:"chargerId"`
	OrderNo    string `json:"orderNo,omitempty"`
	Action     string `json:"action"`
	Result     string `json:"result"`
	Applied    bool   `json:"applied"`
	RecordedAt string `json:"recordedAt"`
}

// CreateStationCommand carries a validated station creation request.
type CreateStationCommand struct {
	AdminID        int64
	Code           string
	Name           string
	Address        string
	LatitudeE6     int64
	LongitudeE6    int64
	IdempotencyKey string
	RequestHash    string
	TraceID        string
}

// RestartCommand carries a validated charger restart request.
type RestartCommand struct {
	AdminID        int64
	ChargerID      int64
	Reason         string
	IdempotencyKey string
	RequestHash    string
	TraceID        string
}

// StationRecord is the created station as stored.
// StationRecord is the station an admin write returns. The JSON names are the
// contract's (the Station schema uses camelCase); without tags Go would emit
// ID/Code/Status and the create-station response would not match the schema it
// is registered under - which is what it did until a status-change test compared
// the response against the contract.
//
// The aggregate fields the Station schema also carries (chargerCount,
// idleChargerCount, minPriceCentPerKwh) are read paths, not something a create
// or a status change computes; they are deliberately absent here.
type StationRecord struct {
	ID          int64  `json:"id"`
	Code        string `json:"code"`
	Name        string `json:"name"`
	Address     string `json:"address"`
	LatitudeE6  int64  `json:"latitudeE6"`
	LongitudeE6 int64  `json:"longitudeE6"`
	Status      string `json:"status"`
}

// TariffView is the charger tariff snapshot the admin API exposes.
type TariffView struct {
	ChargerID            int64  `json:"chargerId"`
	ElectricityPriceCent int64  `json:"electricityPriceCentPerKwh"`
	ServicePriceCent     int64  `json:"servicePriceCentPerKwh"`
	OffPeakPriceCent     *int64 `json:"offPeakElectricityPriceCentPerKwh,omitempty"`
	OffPeakStartHour     *int16 `json:"offPeakStartHour,omitempty"`
	OffPeakEndHour       *int16 `json:"offPeakEndHour,omitempty"`
}

// TariffUpdate carries a validated tariff change. Nil off-peak fields clear
// the time-of-use window (flat tariff).
type TariffUpdate struct {
	AdminID              int64
	ChargerID            int64
	ElectricityPriceCent int64
	ServicePriceCent     int64
	OffPeakPriceCent     *int64
	OffPeakStartHour     *int16
	OffPeakEndHour       *int16
}

// ForceReleaseCommand carries a validated forced-release request (BR-11).
type ForceReleaseCommand struct {
	AdminID        int64
	ChargerID      int64
	Reason         string
	TargetStatus   string // IDLE or DISABLED
	IdempotencyKey string
	RequestHash    string
	TraceID        string
}

// UserDetail is the admin user view with the full registration data
// (SRS 管理端列表: ID、手机号、昵称、余额、注册时间、状态).
type UserDetail struct {
	ID           int64      `json:"id"`
	Phone        string     `json:"phone"`
	DisplayName  string     `json:"displayName"`
	AvatarURL    string     `json:"avatarUrl,omitempty"`
	Status       string     `json:"status"`
	BalanceCent  int64      `json:"balanceCent"`
	RegisteredAt time.Time  `json:"registeredAt"`
	DeletedAt    *time.Time `json:"deletedAt,omitempty"`
}

// LedgerEntry re-exports the wallet ledger shape for the admin user view.
type LedgerEntry = wallet.Entry
type LedgerPage = wallet.EntryPage

// AuditEntry is one operation_logs row for the audit query.
type AuditEntry struct {
	ID           int64     `json:"id"`
	ActorType    string    `json:"actorType"`
	ActorID      string    `json:"actorId"`
	Action       string    `json:"action"`
	ResourceType string    `json:"resourceType"`
	ResourceID   string    `json:"resourceId"`
	RequestID    string    `json:"requestId,omitempty"`
	Payload      string    `json:"payload,omitempty"`
	CreatedAt    time.Time `json:"createdAt"`
}

// AuditPage is one page of the audit trail.
type AuditPage struct {
	Items []AuditEntry `json:"items"`
	Meta  PageMeta     `json:"meta"`
}

// AuditFilter carries validated audit query parameters.
type AuditFilter struct {
	Page         int64
	PageSize     int64
	ActorID      string
	Action       string
	ResourceType string
	ResourceID   string
}

// UserLedgerFilter carries the admin view of one user's ledger.
type UserLedgerFilter struct {
	UserID   int64
	Page     int64
	PageSize int64
	Type     string
}

// ChangeStationStatusCommand carries a validated station status change.
type ChangeStationStatusCommand struct {
	AdminID   int64
	StationID int64
	Status    string
	TraceID   string
}

// ChangeChargerStatusCommand carries a validated charger status change.
type ChangeChargerStatusCommand struct {
	AdminID   int64
	ChargerID int64
	Status    string
	TraceID   string
}

// ChargerStatusRecord is the charger a status change returned.
type ChargerStatusRecord struct {
	ChargerID   int64  `json:"chargerId"`
	ChargerCode string `json:"chargerCode"`
	Status      string `json:"status"`
}

// ErrDeviceCommandNotFound reports a device command the platform has no record
// of. It maps to 404: the identifier is either wrong or the gateway never
// answered, and neither is something the caller can fix by retrying.
var ErrDeviceCommandNotFound = errors.New("admin: device command not found")

// ErrInvalidStatusValue reports a status outside the contract enum.
var ErrInvalidStatusValue = errors.New("admin: invalid status value")

// ErrStationNotFound reports a station id the platform does not have.
var ErrStationNotFound = errors.New("admin: station not found")

// ErrChargerNotFound reports a charger id the platform does not have.
var ErrChargerNotFound = errors.New("admin: charger not found")

// Store persists admin operations. Mutating methods own their transactions
// so business rows, the audit trail and idempotency records commit together.
type Store interface {
	CreateStation(ctx context.Context, command CreateStationCommand) (StationRecord, error)
	ListStations(ctx context.Context, page, pageSize int64, keyword string) (StationPage, error)
	ListChargers(ctx context.Context, filter station.ChargerFilter) (ChargerPage, error)
	ListUsers(ctx context.Context, filter UserFilter) (UserPage, error)
	GetUserDetail(ctx context.Context, userID int64) (UserDetail, error)
	ListUserLedger(ctx context.Context, filter UserLedgerFilter) (LedgerPage, error)
	ListOrders(ctx context.Context, filter AdminOrderFilter) (OrderPage, error)
	RestartCharger(ctx context.Context, command RestartCommand) (Command, error)
	FindDeviceCommand(ctx context.Context, commandID string) (DeviceCommand, error)
	ChangeStationStatus(ctx context.Context, command ChangeStationStatusCommand) (StationRecord, error)
	ChangeChargerStatus(ctx context.Context, command ChangeChargerStatusCommand) (ChargerStatusRecord, error)
	GetTariff(ctx context.Context, chargerID int64) (TariffView, error)
	UpdateStation(ctx context.Context, command UpdateStationCommand) (StationRecord, error)
	CreateChargers(ctx context.Context, command CreateChargersCommand) (ChargerBatchResult, error)
	CreateUser(ctx context.Context, command CreateUserCommand) (UserRecord, error)
	CreateUsers(ctx context.Context, command CreateUsersCommand) (UserBatchResult, error)
	GlobalTariff(ctx context.Context) ([]GlobalTariff, error)
	UpdateGlobalTariff(ctx context.Context, update GlobalTariffUpdate) (GlobalTariffResult, error)

	// The statistics reads. They are aggregates rather than pages, so they are
	// separate methods instead of filters on the list queries: a list query
	// returns rows to render, and these return counts and sums that never exist
	// as a row.
	RevenueTotals(ctx context.Context, query RevenueQuery) (RevenueTotals, error)
	RevenueBuckets(ctx context.Context, query RevenueQuery) ([]RevenueBucket, error)
	ChargerCounts(ctx context.Context, stationID int64) (ChargerCounts, error)
	FleetCounts(ctx context.Context) (FleetCounts, error)
	UpdateTariff(ctx context.Context, update TariffUpdate) (TariffView, error)
	ForceRelease(ctx context.Context, command ForceReleaseCommand) (StationRecordCharger, error)
	ListAudit(ctx context.Context, filter AuditFilter) (AuditPage, error)
}

// StationRecordCharger reports the charger after a forced release.
type StationRecordCharger struct {
	ChargerID   int64  `json:"chargerId"`
	ChargerCode string `json:"chargerCode"`
	OrderNo     string `json:"orderNo,omitempty"`
	Status      string `json:"status"`
}

// Service validates commands and delegates persistence to a Store.
type Service struct {
	store    Store
	accounts accountManagementStore
	clock    func() time.Time
}

// NewService wires the service to its store.
func NewService(store Store) (*Service, error) {
	if store == nil {
		return nil, errors.New("admin: store is required")
	}
	service := &Service{store: store, clock: time.Now}
	service.accounts, _ = store.(accountManagementStore)
	return service, nil
}

// Create validates and creates a station in status OPEN.
func (s *Service) Create(ctx context.Context, command CreateStationCommand) (StationRecord, error) {
	if command.AdminID < 1 {
		return StationRecord{}, ErrInvalidAdminActor
	}
	if !stationCodePattern.MatchString(command.Code) {
		return StationRecord{}, fmt.Errorf("%w: station code", ErrInvalidStationFilter)
	}
	if len(command.Name) < 1 || len(command.Name) > 100 {
		return StationRecord{}, fmt.Errorf("%w: station name length", ErrInvalidStationFilter)
	}
	if len(command.Address) < 1 || len(command.Address) > 255 {
		return StationRecord{}, fmt.Errorf("%w: station address length", ErrInvalidStationFilter)
	}
	if command.LatitudeE6 < -90_000_000 || command.LatitudeE6 > 90_000_000 ||
		command.LongitudeE6 < -180_000_000 || command.LongitudeE6 > 180_000_000 {
		return StationRecord{}, fmt.Errorf("%w: coordinates out of range", ErrInvalidStationFilter)
	}
	return s.store.CreateStation(ctx, command)
}

// GetTariff returns the charger tariff view.
func (s *Service) GetTariff(ctx context.Context, chargerID int64) (TariffView, error) {
	if chargerID < 1 {
		return TariffView{}, ErrChargerUnavailable
	}
	return s.store.GetTariff(ctx, chargerID)
}

// UpdateTariff applies a tariff change under audit (价格调整). The order
// domain snapshots the tariff at charging start, so running orders are not
// affected — exactly what the snapshot semantics guarantee.
func (s *Service) UpdateTariff(ctx context.Context, update TariffUpdate) (TariffView, error) {
	if update.AdminID < 1 {
		return TariffView{}, ErrInvalidAdminActor
	}
	if update.ChargerID < 1 {
		return TariffView{}, ErrChargerUnavailable
	}
	if update.ElectricityPriceCent < 0 || update.ServicePriceCent < 0 {
		return TariffView{}, ErrInvalidTariff
	}
	// The off-peak triple (price, window start, window end) is either fully
	// absent (flat tariff) or fully present and valid — partial windows are
	// rejected so a tariff can never end up with a dangling boundary.
	offPeakFields := 0
	if update.OffPeakPriceCent != nil {
		offPeakFields++
	}
	if update.OffPeakStartHour != nil {
		offPeakFields++
	}
	if update.OffPeakEndHour != nil {
		offPeakFields++
	}
	if offPeakFields == 3 {
		if *update.OffPeakPriceCent < 0 ||
			*update.OffPeakStartHour < 0 || *update.OffPeakStartHour > 23 ||
			*update.OffPeakEndHour < 0 || *update.OffPeakEndHour > 23 ||
			*update.OffPeakStartHour == *update.OffPeakEndHour {
			return TariffView{}, ErrInvalidTariff
		}
	} else if offPeakFields != 0 {
		return TariffView{}, ErrInvalidTariff
	}
	return s.store.UpdateTariff(ctx, update)
}

// ForceRelease forcibly releases a charger held by a CREATED or STARTING
// order (BR-11): the order is cancelled, the charger moves to the requested
// target status, and the action is audited. Charging devices are rejected —
// they must go through the controlled stop flow first.
func (s *Service) ForceRelease(ctx context.Context, command ForceReleaseCommand) (StationRecordCharger, error) {
	if command.AdminID < 1 {
		return StationRecordCharger{}, ErrInvalidAdminActor
	}
	if command.ChargerID < 1 {
		return StationRecordCharger{}, ErrChargerUnavailable
	}
	if len(command.Reason) < 2 || len(command.Reason) > 200 {
		return StationRecordCharger{}, ErrInvalidReason
	}
	if command.TargetStatus != "IDLE" && command.TargetStatus != "DISABLED" {
		return StationRecordCharger{}, ErrInvalidTariff
	}
	return s.store.ForceRelease(ctx, command)
}

// UserDetail returns the administrative user view.
func (s *Service) UserDetail(ctx context.Context, userID int64) (UserDetail, error) {
	if userID < 1 {
		return UserDetail{}, ErrInvalidAdminActor
	}
	return s.store.GetUserDetail(ctx, userID)
}

// UserLedger returns one page of a user's ledger for the admin view.
func (s *Service) UserLedger(ctx context.Context, filter UserLedgerFilter) (LedgerPage, error) {
	if filter.UserID < 1 {
		return LedgerPage{}, ErrInvalidAdminActor
	}
	if filter.Page < 1 || filter.PageSize < 1 || filter.PageSize > 100 {
		return LedgerPage{}, ErrInvalidLedgerFilter
	}
	switch filter.Type {
	case "", wallet.TypeTopUp, wallet.TypeCharge, wallet.TypeRefund, wallet.TypeAdjustment:
	default:
		return LedgerPage{}, ErrInvalidLedgerFilter
	}
	return s.store.ListUserLedger(ctx, filter)
}

// Audit returns one page of the operation audit trail.
func (s *Service) Audit(ctx context.Context, filter AuditFilter) (AuditPage, error) {
	if filter.Page < 1 || filter.PageSize < 1 || filter.PageSize > 100 {
		return AuditPage{}, ErrInvalidLedgerFilter
	}
	return s.store.ListAudit(ctx, filter)
}

// Restart validates and requests a device restart for an idle or faulted
// charger. Charging and disabled devices are rejected (BR-11).
func (s *Service) Restart(ctx context.Context, command RestartCommand) (Command, error) {
	if command.AdminID < 1 {
		return Command{}, ErrInvalidAdminActor
	}
	if command.ChargerID < 1 {
		return Command{}, ErrChargerUnavailable
	}
	if len(command.Reason) < 2 || len(command.Reason) > 200 {
		return Command{}, ErrInvalidReason
	}
	return s.store.RestartCharger(ctx, command)
}

// NewCommandID mints a device command business number: CMD + UTC timestamp
// + 8 random hex chars. The audit trail and the outbox event carry it so
// the command can be traced end to end.
// ChangeStationStatus validates the target status and delegates the transition
// check to the store, which reads the current status under a row lock: whether a
// transition is legal depends on the state the row is in right now, not on the
// state a caller read a moment ago.
func (s *Service) ChangeStationStatus(ctx context.Context, command ChangeStationStatusCommand) (StationRecord, error) {
	if command.AdminID < 1 || command.StationID < 1 {
		return StationRecord{}, ErrStationNotFound
	}
	if !station.IsStationStatus(command.Status) {
		return StationRecord{}, ErrInvalidStatusValue
	}
	return s.store.ChangeStationStatus(ctx, command)
}

// ChangeChargerStatus validates the target status and delegates the transition
// check to the store for the same reason.
func (s *Service) ChangeChargerStatus(ctx context.Context, command ChangeChargerStatusCommand) (ChargerStatusRecord, error) {
	if command.AdminID < 1 || command.ChargerID < 1 {
		return ChargerStatusRecord{}, ErrChargerNotFound
	}
	if !station.IsChargerStatus(command.Status) {
		return ChargerStatusRecord{}, ErrInvalidStatusValue
	}
	return s.store.ChangeChargerStatus(ctx, command)
}

// DeviceCommand returns what the platform recorded for one device command. The
// identifier is the commandId the restart endpoint returned, so an operator can
// follow a restart from submission to the receipt that applied it.
func (s *Service) DeviceCommand(ctx context.Context, commandID string) (DeviceCommand, error) {
	commandID = strings.TrimSpace(commandID)
	if commandID == "" {
		return DeviceCommand{}, ErrDeviceCommandNotFound
	}
	return s.store.FindDeviceCommand(ctx, commandID)
}

func NewCommandID(now time.Time) (string, error) {
	buffer := make([]byte, 4)
	if _, err := rand.Read(buffer); err != nil {
		return "", fmt.Errorf("admin: generate command number: %w", err)
	}
	return "CMD" + now.UTC().Format("20060102150405") + hex.EncodeToString(buffer), nil
}
