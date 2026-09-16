package order

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"time"
)

// Sentinel errors mapped by the HTTP layer to the shared error-code registry.
var (
	// ErrOrderNotFound maps to 404 NOT_FOUND. It also covers orders that
	// exist but belong to another user, so ownership is not enumerable.
	ErrOrderNotFound = errors.New("order: order not found")
	// ErrChargerUnavailable maps to 409 CHARGER_UNAVAILABLE (code 8). It
	// covers missing chargers and chargers or stations that are not open.
	ErrChargerUnavailable = errors.New("order: charger is unavailable")
	// ErrInsufficientBalance maps to 409 INSUFFICIENT_BALANCE (code 7,
	// BR-04 minimum start balance).
	ErrInsufficientBalance = errors.New("order: insufficient balance for the minimum start amount")
	// ErrActiveFlowExists maps to 409 ACTIVE_FLOW_EXISTS (code 9, UC-U-07
	// flow uniqueness).
	ErrActiveFlowExists = errors.New("order: user already has an active flow")
	// ErrIdempotencyConflict maps to 409 IDEMPOTENCY_CONFLICT (code 14):
	// the key was already used for a different request.
	ErrIdempotencyConflict = errors.New("order: idempotency key reused for a different request")
	// ErrIdempotencyInProgress maps to 409 IDEMPOTENCY_CONFLICT as well:
	// an identical request is still executing.
	ErrIdempotencyInProgress = errors.New("order: identical request is still in progress")
	// ErrInvalidOrderNo reports an order number outside the contract bounds.
	ErrInvalidOrderNo = errors.New("order: invalid order number")
	// ErrInvalidPagination reports page/pageSize outside contract bounds.
	ErrInvalidPagination = errors.New("order: invalid pagination parameters")
	// ErrInvalidStatusFilter reports a status outside the contract enum.
	ErrInvalidStatusFilter = errors.New("order: invalid status filter")
	// ErrInvalidPaymentStatus reports a payment state outside the enum.
	ErrInvalidPaymentStatus = errors.New("order: invalid payment status")
	// ErrInvalidSort reports a sort value outside {createdAt, -createdAt}.
	ErrInvalidSort = errors.New("order: invalid sort value")
	// ErrInvalidTimeWindow reports fromAt >= toAt, a window that can never match.
	ErrInvalidTimeWindow = errors.New("order: invalid creation-time window")
	// ErrInvalidChargerID reports a chargerId below 1 in the request body.
	ErrInvalidChargerID = errors.New("order: invalid charger id")
	// ErrUserFrozen maps to 403 USER_FROZEN: the account was disabled after
	// the order was created and must not be allowed to start charging.
	ErrUserFrozen = errors.New("order: account is disabled")
	// ErrDebtOutstanding maps to 409 DEBT_OUTSTANDING: the user already has
	// an unsettled (PENDING) completed order; at most one unsettled order
	// may exist per user.
	ErrDebtOutstanding = errors.New("order: user has an unsettled order")
	// ErrReservationExpired maps to 409 RESERVATION_EXPIRED. A reservation
	// that reached its deadline is atomically expired and its charger is
	// released before this error is returned.
	ErrReservationExpired = errors.New("order: reservation has expired")

	// Device receipt errors (BE-I-02). A receipt is a business fact the charger
	// gateway reports, so these are all client errors: none of them is fixed by
	// retrying the same payload, and none of them may change an order.
	//
	// ErrInvalidFactTime maps to 400: the fact time is missing, is not UTC, or is
	// implausibly far in the future.
	ErrInvalidFactTime = errors.New("order: invalid device fact time")
	// ErrFactTimeOutOfOrder maps to 409: the device reports a stop that is earlier
	// than the recorded start, or a receipt the state machine cannot accept yet.
	ErrFactTimeOutOfOrder = errors.New("order: device fact time is out of order")
	// ErrChargerOrderMismatch maps to 409: the receipt names a charger that does
	// not belong to the order it claims.
	ErrChargerOrderMismatch = errors.New("order: charger does not belong to the order")
	// ErrInvalidReceiptID maps to 400: the receipt has no usable idempotency id.
	ErrInvalidReceiptID = errors.New("order: invalid receipt event id")
)

// DefaultFactTimeSkew is how far a device fact time may run ahead of the server
// clock before a receipt is rejected (frozen default, configurable).
const DefaultFactTimeSkew = 5 * time.Minute

// Receipt id bounds mirror the contract for the receipt identifier.
const (
	minReceiptIDLength = 8
	maxReceiptIDLength = 128
)

// Order is the contract Order payload. Timestamps marshal as UTC RFC3339.
// PaymentStatus tracks the pending-payment lifecycle (UC-U-09): PENDING
// from completion until confirmation, then PAID or PARTIAL_PAID.
type Order struct {
	OrderNo   string `json:"orderNo"`
	UserID    int64  `json:"userId"`
	StationID int64  `json:"stationId"`
	ChargerID int64  `json:"chargerId"`
	Status    string `json:"status"`
	// ReservedUntil is present while the order is in CREATED, which is the
	// concrete-device reservation stage. The user must start or cancel before
	// this UTC deadline.
	ReservedUntil *time.Time `json:"reservedUntil,omitempty"`
	AmountCent    int64      `json:"amountCent"`
	PaidCent      int64      `json:"paidCent,omitempty"`
	PaymentStatus string     `json:"paymentStatus,omitempty"`
	EnergyWh      int64      `json:"energyWh,omitempty"`
	// StartedAt is the device's fact time for the start (CHARGE_STARTED receipt).
	// Absent until the device confirms the start.
	StartedAt *time.Time `json:"startedAt,omitempty"`
	// ChargerPowerWatt is the rated power of the bound charger, and
	// UnitPriceCentPerKwh the price per kWh that applies right now (the frozen
	// snapshot resolved against the off-peak window in the billing timezone).
	// They are the estimate basis a client may use before the first meter reading
	// arrives; the reading itself is MeteredEnergyWh/MeteredAmountCent.
	ChargerPowerWatt    *int64 `json:"chargerPowerWatt,omitempty"`
	UnitPriceCentPerKwh *int64 `json:"unitPriceCentPerKwh,omitempty"`
	// MeteredEnergyWh and MeteredAmountCent are the running meter while the
	// charger is charging (the latest CHARGE_PROGRESS reading) and what that
	// energy costs at the frozen snapshot. They stay separate from
	// EnergyWh/AmountCent, which are the settled figures the bill is built from:
	// a live reading must never leak into settled money. MeteredAt is the
	// reading's own device fact time.
	MeteredEnergyWh   *int64     `json:"meteredEnergyWh,omitempty"`
	MeteredAmountCent *int64     `json:"meteredAmountCent,omitempty"`
	MeteredAt         *time.Time `json:"meteredAt,omitempty"`
	CreatedAt         time.Time  `json:"createdAt"`
	UpdatedAt         time.Time  `json:"updatedAt,omitempty"`
}

// PageMeta is the contract pagination metadata.
type PageMeta struct {
	Page     int64 `json:"page"`
	PageSize int64 `json:"pageSize"`
	Total    int64 `json:"total"`
}

// OrderPage is one page of orders.
type OrderPage struct {
	Items []Order  `json:"items"`
	Meta  PageMeta `json:"meta"`
}

// CreateOrderCommand carries a validated creation request.
type CreateOrderCommand struct {
	UserID         int64
	ChargerID      int64
	IdempotencyKey string
	RequestHash    string
	TraceID        string
}

// TransitionCommand carries a validated start/stop request.
type TransitionCommand struct {
	UserID         int64
	OrderNo        string
	IdempotencyKey string
	RequestHash    string
	TraceID        string
}

// ConfirmStartCommand records the device-side confirmation of charging.
//
// OccurredAt, EventID, RequestHash and ChargerID are the receipt fields the
// gateway reports (BE-I-02). OccurredAt is the business fact time: it is what
// goes into started_at, so an offline report is billed in the window it really
// happened in rather than in the window the platform happened to receive it.
// EventID is the receipt's idempotency key and RequestHash its payload digest;
// both are required on the device path, because a duplicate receipt must return
// the first result instead of applying itself twice.
type ConfirmStartCommand struct {
	OrderNo string
	// ChargerID is the charger the device claims; it must match the order's
	// charger, otherwise the receipt is rejected.
	ChargerID int64
	// OccurredAt is the device-side fact time, in UTC.
	OccurredAt time.Time
	// EventID and RequestHash drive receipt idempotency. They are set by the
	// gateway endpoint; a direct store call without them is not deduplicated.
	EventID     string
	RequestHash string
	TraceID     string
}

// ConfirmStopCommand records the device-side stop with the metered energy.
// The optional meter readings (device register values in watt-hours) are
// validated when present: end >= start, and energy must equal their
// difference, so a fabricated energy value cannot pass unchecked.
//
// OccurredAt is the stop fact time: it is written to stopped_at / completed_at
// and it is the end of the interval the time-of-use bill is computed over.
type ConfirmStopCommand struct {
	OrderNo      string
	ChargerID    int64
	EnergyWh     int64
	MeterStartWh *int64
	MeterEndWh   *int64
	OccurredAt   time.Time
	EventID      string
	RequestHash  string
	TraceID      string
}

// ConfirmProgressCommand carries one running-meter reading from the charger.
//
// Readings are absolute (energy delivered so far) and monotonic: the store keeps
// the latest one and ignores a lower reading, which is what makes the receipt
// safe to retry without an idempotency key. Progress never settles an order -
// the stop receipt still decides the bill.
type ConfirmProgressCommand struct {
	OrderNo     string
	ChargerID   int64
	EnergyWh    int64
	OccurredAt  time.Time
	EventID     string
	RequestHash string
	TraceID     string
}

// StopRecoveryPolicy bounds the STOP recovery sweep: how many STOP_CHARGING
// commands one order may accumulate, and how long the sweep waits between two
// of them.
type StopRecoveryPolicy struct {
	// MaxAttempts counts every STOP_CHARGING command issued for the order,
	// including the one the user's stop produced.
	MaxAttempts int
	// Backoff is the minimum age of the newest STOP_CHARGING command before
	// another one is issued.
	Backoff time.Duration
}

// StopRecoveryResult reports one sweep.
type StopRecoveryResult struct {
	// Reissued counts the orders that were sent a fresh STOP_CHARGING.
	Reissued int
	// Escalated counts the orders that reached the attempt limit and were
	// handed to an operator in this sweep.
	Escalated int
	// Waiting counts the orders that are still inside the backoff window.
	Waiting int
}

// ListFilter carries validated list parameters for the order history:
// status, station, payment state and the creation-time window.
type ListFilter struct {
	UserID        int64
	Page          int64
	PageSize      int64
	Status        string
	StationID     int64
	StationIDSet  bool
	PaymentStatus string
	CreatedFrom   *time.Time
	CreatedTo     *time.Time
	// Sort is the order of the result page. The contract allows "createdAt" and
	// "-createdAt" (newest first, the default); anything else is rejected
	// rather than silently ignored, so a typo cannot look like a valid query.
	Sort string
}

// SortCreatedAtAsc and SortCreatedAtDesc are the two sort values the contract
// registers for the order list.
const (
	SortCreatedAtAsc  = "createdAt"
	SortCreatedAtDesc = "-createdAt"
)

// SettleCommand carries a validated confirmation request (UC-U-09).
type SettleCommand struct {
	TransitionCommand
}

// Store persists orders inside PostgreSQL transactions. Every mutating
// method owns its transaction so business rows, charger status, idempotency
// records and outbox events commit atomically or not at all.
type Store interface {
	CreateOrder(ctx context.Context, command CreateOrderCommand) (Order, error)
	StartCharging(ctx context.Context, command TransitionCommand) (Order, error)
	StopCharging(ctx context.Context, command TransitionCommand) (Order, error)
	CancelOrder(ctx context.Context, command TransitionCommand) (Order, error)
	ConfirmStart(ctx context.Context, command ConfirmStartCommand) (Order, error)
	ConfirmStop(ctx context.Context, command ConfirmStopCommand) (Order, error)
	ConfirmProgress(ctx context.Context, command ConfirmProgressCommand) (Order, error)
	// SettleOrder executes the UC-U-09 user confirmation of a completed
	// order: deduct the pending bill from the wallet and record the payment.
	SettleOrder(ctx context.Context, command SettleCommand) (Order, error)
	// ExpireStaleOrders moves abandoned CREATED orders to EXPIRED and stuck
	// STARTING orders to FAILED. STARTING orders queue a compensating
	// device command instead of releasing their charger immediately.
	ExpireStaleOrders(ctx context.Context, olderThan time.Duration) (int, error)
	// ReleaseOrphanedChargers frees OCCUPIED chargers whose orders are all
	// terminal and that never sent a start command; it runs after the
	// compensation commands are queued.
	ReleaseOrphanedChargers(ctx context.Context) (int, error)
	// CompleteChargerCommand applies the device-side outcome of a charger
	// command: COMPLETED returns the charger to IDLE, any other outcome parks
	// it in FAULT. The B-line worker calls it when CHARGER_COMMAND_COMPLETED
	// arrives.
	CompleteChargerCommand(ctx context.Context, chargerID int64, action, result string) (bool, error)
	// RecordChargerCommandResult applies the device outcome and appends the
	// CHARGER_COMMAND_COMPLETED event in one transaction. The B-line
	// dispatcher calls it after the gateway answers, so the event that a
	// consumer acts on is always written with the state change it describes.
	//
	// orderNo attributes the outcome to the order whose command was sent. It is
	// what lets an explicit START failure move the order to FAILED in the same
	// transaction as the charger's FAULT, instead of leaving an order stuck in
	// STARTING with nobody able to tell that the device refused.
	RecordChargerCommandResult(ctx context.Context, commandNo, orderNo string, chargerID int64, action, result, traceID string) (bool, error)
	// ReissueStopCommands is the STOP recovery sweep (BE-I-02): it re-sends
	// STOP_CHARGING with a new command id to orders that are stuck in STOPPING
	// with a faulty charger, up to the policy limit, and hands the rest to an
	// operator. It never releases a charger and never settles an order, because
	// a device that may still be charging must not be closed automatically.
	ReissueStopCommands(ctx context.Context, policy StopRecoveryPolicy) (StopRecoveryResult, error)
	GetOrderByNo(ctx context.Context, userID int64, orderNo string) (Order, error)
	ListOrdersByUser(ctx context.Context, filter ListFilter) (OrderPage, error)
}

// Service validates commands and delegates persistence to a Store.
type Service struct {
	store Store
	clock func() time.Time
	// factTimeSkew is how far a device fact time may run ahead of the server
	// clock before the receipt is rejected. Zero means the frozen default.
	factTimeSkew time.Duration
}

// NewService wires the service to its store.
func NewService(store Store) (*Service, error) {
	if store == nil {
		return nil, errors.New("order: store is required")
	}
	return &Service{store: store, clock: time.Now, factTimeSkew: DefaultFactTimeSkew}, nil
}

// SetFactTimeSkew configures how far a device fact time may run ahead of the
// server clock (NCS_CHARGER_EVENT_MAX_FUTURE_SKEW). A non-positive value is
// rejected so a misconfiguration cannot silently accept a future-dated receipt
// that would bill an order in a window that has not happened yet.
func (s *Service) SetFactTimeSkew(skew time.Duration) error {
	if skew <= 0 {
		return errors.New("order: fact time skew must be positive")
	}
	s.factTimeSkew = skew
	return nil
}

// Create validates and creates a 15-minute device reservation in status
// CREATED. Starting the charge is a separate, explicit user action.
func (s *Service) Create(ctx context.Context, command CreateOrderCommand) (Order, error) {
	if command.UserID < 1 || command.ChargerID < 1 {
		return Order{}, ErrInvalidChargerID
	}
	return s.store.CreateOrder(ctx, command)
}

// Start moves an unexpired CREATED reservation to STARTING after a user start
// request.
func (s *Service) Start(ctx context.Context, command TransitionCommand) (Order, error) {
	if err := validateTransitionCommand(command); err != nil {
		return Order{}, err
	}
	return s.store.StartCharging(ctx, command)
}

// Stop moves a CHARGING order to STOPPING after a user stop request.
func (s *Service) Stop(ctx context.Context, command TransitionCommand) (Order, error) {
	if err := validateTransitionCommand(command); err != nil {
		return Order{}, err
	}
	return s.store.StopCharging(ctx, command)
}

// Cancel moves a CREATED or STARTING order to CANCELLED and releases the
// charger. The endpoint is idempotent through the same key mechanism as the
// other transitions (UC-U-07).
func (s *Service) Cancel(ctx context.Context, command TransitionCommand) (Order, error) {
	if err := validateTransitionCommand(command); err != nil {
		return Order{}, err
	}
	return s.store.CancelOrder(ctx, command)
}

// ExpireStaleOrders is the janitor entry point; see Store.ExpireStaleOrders.
func (s *Service) ExpireStaleOrders(ctx context.Context, olderThan time.Duration) (int, error) {
	return s.store.ExpireStaleOrders(ctx, olderThan)
}

// ReleaseOrphanedChargers is the janitor entry point for chargers left
// occupied by terminal orders that never sent a start command.
func (s *Service) ReleaseOrphanedChargers(ctx context.Context) (int, error) {
	return s.store.ReleaseOrphanedChargers(ctx)
}

// CompleteChargerCommand applies the device-side outcome of a charger
// command; see Store.CompleteChargerCommand.
func (s *Service) CompleteChargerCommand(ctx context.Context, chargerID int64, action, result string) (bool, error) {
	if chargerID < 1 {
		return false, nil
	}
	return s.store.CompleteChargerCommand(ctx, chargerID, action, result)
}

// RecordChargerCommandResult applies a device outcome and emits the completion
// event atomically; see Store.RecordChargerCommandResult.
func (s *Service) RecordChargerCommandResult(ctx context.Context, commandNo, orderNo string, chargerID int64, action, result, traceID string) (bool, error) {
	if chargerID < 1 || strings.TrimSpace(commandNo) == "" {
		return false, nil
	}
	return s.store.RecordChargerCommandResult(ctx, commandNo, orderNo, chargerID, action, result, traceID)
}

// Settle confirms a completed order and collects the pending bill
// (UC-U-09 用户确认扣款).
func (s *Service) Settle(ctx context.Context, command SettleCommand) (Order, error) {
	if err := validateTransitionCommand(command.TransitionCommand); err != nil {
		return Order{}, err
	}
	return s.store.SettleOrder(ctx, command)
}

// ConfirmStart moves a STARTING order to CHARGING on device confirmation.
//
// The device path is the charger gateway, which authenticates with a service
// token rather than a session; the receipt's id is its idempotency key and the
// transition itself is guarded by the order row lock.
func (s *Service) ConfirmStart(ctx context.Context, command ConfirmStartCommand) (Order, error) {
	if err := validateOrderNo(command.OrderNo); err != nil {
		return Order{}, err
	}
	if err := s.validateReceipt(command.ChargerID, command.OccurredAt, command.EventID); err != nil {
		return Order{}, err
	}
	return s.store.ConfirmStart(ctx, command)
}

// ConfirmProgress applies one running-meter reading from the charger.
//
// It only applies to an order that is CHARGING. The charger id and the fact time
// are validated exactly like the other receipts, because the reading decides what
// the app shows as the amount accrued so far.
func (s *Service) ConfirmProgress(ctx context.Context, command ConfirmProgressCommand) (Order, error) {
	if err := validateOrderNo(command.OrderNo); err != nil {
		return Order{}, err
	}
	if err := s.validateReceipt(command.ChargerID, command.OccurredAt, command.EventID); err != nil {
		return Order{}, err
	}
	if command.EnergyWh < 0 {
		return Order{}, errors.New("order: metered energy must not be negative")
	}
	return s.store.ConfirmProgress(ctx, command)
}

// ConfirmStop moves a STOPPING order to COMPLETED, computing the billed
// amount from the metered energy and releasing the charger.
func (s *Service) ConfirmStop(ctx context.Context, command ConfirmStopCommand) (Order, error) {
	if err := validateOrderNo(command.OrderNo); err != nil {
		return Order{}, err
	}
	if err := s.validateReceipt(command.ChargerID, command.OccurredAt, command.EventID); err != nil {
		return Order{}, err
	}
	if command.EnergyWh < 0 {
		return Order{}, errors.New("order: metered energy must not be negative")
	}
	if command.MeterStartWh != nil && command.MeterEndWh != nil {
		if *command.MeterEndWh < *command.MeterStartWh {
			return Order{}, errors.New("order: meter end reading is below the start reading")
		}
		if command.EnergyWh != *command.MeterEndWh-*command.MeterStartWh {
			return Order{}, errors.New("order: metered energy does not match the meter readings")
		}
	}
	return s.store.ConfirmStop(ctx, command)
}

// validateReceipt enforces the frozen receipt contract before any state is
// touched: the receipt must name a charger, carry a usable idempotency id, and
// report a fact time that is UTC and not in the future.
//
// The fact time is not an audit field. It decides started_at, stopped_at and
// the time-of-use window the order is billed in, so a skewed or locally zoned
// timestamp would produce a wrong bill rather than a cosmetic difference.
func (s *Service) validateReceipt(chargerID int64, occurredAt time.Time, eventID string) error {
	if chargerID < 1 {
		return ErrInvalidChargerID
	}
	id := strings.TrimSpace(eventID)
	if len(id) < minReceiptIDLength || len(id) > maxReceiptIDLength {
		return ErrInvalidReceiptID
	}
	return s.ValidateFactTime(occurredAt)
}

// ValidateFactTime checks one device fact time against the contract: present,
// UTC (zero offset) and no further ahead of the server clock than the
// configured skew.
func (s *Service) ValidateFactTime(occurredAt time.Time) error {
	if occurredAt.IsZero() {
		return fmt.Errorf("%w: occurredAt is required", ErrInvalidFactTime)
	}
	if _, offset := occurredAt.Zone(); offset != 0 {
		return fmt.Errorf("%w: occurredAt must be UTC", ErrInvalidFactTime)
	}
	skew := s.factTimeSkew
	if skew <= 0 {
		skew = DefaultFactTimeSkew
	}
	if occurredAt.After(s.clock().Add(skew)) {
		return fmt.Errorf("%w: occurredAt is more than %s in the future", ErrInvalidFactTime, skew)
	}
	return nil
}

// ReissueStopCommands is the janitor entry point of the STOP recovery path; see
// Store.ReissueStopCommands.
func (s *Service) ReissueStopCommands(ctx context.Context, policy StopRecoveryPolicy) (StopRecoveryResult, error) {
	if policy.MaxAttempts < 1 {
		return StopRecoveryResult{}, errors.New("order: stop recovery needs an attempt limit")
	}
	if policy.Backoff <= 0 {
		return StopRecoveryResult{}, errors.New("order: stop recovery needs a positive backoff")
	}
	return s.store.ReissueStopCommands(ctx, policy)
}

// Get returns one order of the calling user.
func (s *Service) Get(ctx context.Context, userID int64, orderNo string) (Order, error) {
	if err := validateOrderNo(orderNo); err != nil {
		return Order{}, err
	}
	return s.store.GetOrderByNo(ctx, userID, orderNo)
}

// List returns one page of the calling user's orders.
func (s *Service) List(ctx context.Context, filter ListFilter) (OrderPage, error) {
	if filter.UserID < 1 {
		return OrderPage{}, ErrOrderNotFound
	}
	if filter.Page < 1 || filter.PageSize < 1 || filter.PageSize > 100 {
		return OrderPage{}, ErrInvalidPagination
	}
	if filter.Status != "" && !IsValidStatus(filter.Status) {
		return OrderPage{}, ErrInvalidStatusFilter
	}
	if filter.PaymentStatus != "" && filter.PaymentStatus != "PENDING" && filter.PaymentStatus != "PAID" && filter.PaymentStatus != "PARTIAL_PAID" {
		return OrderPage{}, ErrInvalidPaymentStatus
	}
	// The creation-time window and the sort value are part of the registered
	// contract for this endpoint (the front end sends fromAt/toAt/sort), so they
	// are validated here as well as parsed at the HTTP edge: a service called
	// directly must not accept a window that can never match or an unknown sort.
	if filter.CreatedFrom != nil && filter.CreatedTo != nil && !filter.CreatedFrom.Before(*filter.CreatedTo) {
		return OrderPage{}, ErrInvalidTimeWindow
	}
	switch filter.Sort {
	case "", SortCreatedAtAsc, SortCreatedAtDesc:
	default:
		return OrderPage{}, ErrInvalidSort
	}

	result, err := s.store.ListOrdersByUser(ctx, filter)
	if err != nil {
		return OrderPage{}, err
	}
	if result.Items == nil {
		result.Items = []Order{}
	}
	return result, nil
}

const (
	minOrderNoLength = 8
	maxOrderNoLength = 64
)

func validateOrderNo(orderNo string) error {
	if len(orderNo) < minOrderNoLength || len(orderNo) > maxOrderNoLength {
		return ErrInvalidOrderNo
	}
	return nil
}

func validateTransitionCommand(command TransitionCommand) error {
	if err := validateOrderNo(command.OrderNo); err != nil {
		return err
	}
	if command.UserID < 1 {
		return ErrOrderNotFound
	}
	return nil
}
