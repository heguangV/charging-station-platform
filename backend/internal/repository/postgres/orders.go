package postgres

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"strconv"
	"strings"
	"time"

	"github.com/heguangV/charging-station-platform/backend/internal/admin"
	"github.com/heguangV/charging-station-platform/backend/internal/order"
)

// idempotencyTTL bounds how long a stored idempotency result is replayable.
const idempotencyTTL = 24 * time.Hour

// OrderStore implements order.Store. Every mutating method runs one
// transaction covering business rows, charger status, the idempotency record
// and the outbox events — the unit of work the outbox publisher later reads.
type OrderStore struct {
	db    *sql.DB
	clock func() time.Time
}

// NewOrderStore binds the store to a connection pool.
func NewOrderStore(db *sql.DB) (*OrderStore, error) {
	if db == nil {
		return nil, errors.New("postgres: order store requires a database")
	}
	return &OrderStore{db: db, clock: time.Now}, nil
}

// activeStatusSQL inlines the fixed active-status list from the order
// package; the values are code constants, not user input, and must keep
// matching the partial unique index predicates.
const activeStatusSQL = `('CREATED', 'STARTING', 'CHARGING', 'STOPPING')`

func idempotencyScope(operation string, userID int64, orderNo string) string {
	if orderNo == "" {
		return fmt.Sprintf("user:%d:%s", userID, operation)
	}
	return fmt.Sprintf("user:%d:%s:%s", userID, operation, orderNo)
}

// claimIdempotency claims the (scope, key) slot for this request inside the
// transaction. A fresh claim returns (nil, false, nil). A SUCCEEDED record
// with the same request hash replays its stored response body; anything else
// is a conflict or a concurrent in-flight duplicate.
func (s *OrderStore) claimIdempotency(tx *sql.Tx, ctx context.Context, scope, key, requestHash string) ([]byte, bool, error) {
	tag, err := tx.ExecContext(ctx, `INSERT INTO idempotency_records (scope, idempotency_key, request_hash, status, expires_at)
VALUES ($1, $2, $3, 'IN_PROGRESS', $4)
ON CONFLICT (scope, idempotency_key) DO NOTHING`,
		scope, key, requestHash, s.clock().Add(idempotencyTTL))
	if err != nil {
		return nil, false, err
	}
	if affected, err := tag.RowsAffected(); err == nil && affected == 1 {
		return nil, false, nil
	}

	var storedHash, status string
	var body []byte
	var expiresAt time.Time
	err = tx.QueryRowContext(ctx, `SELECT request_hash, status, response_body, expires_at
FROM idempotency_records
WHERE scope = $1 AND idempotency_key = $2
FOR UPDATE`, scope, key).Scan(&storedHash, &status, &body, &expiresAt)
	if errors.Is(err, sql.ErrNoRows) {
		// The row vanished between conflict and select; treat as in-flight.
		return nil, false, order.ErrIdempotencyInProgress
	}
	if err != nil {
		return nil, false, err
	}
	if s.clock().After(expiresAt) {
		// The record's replay window is over: drop it and treat this request
		// as a fresh claim instead of replaying or conflicting forever.
		if _, err := tx.ExecContext(ctx, `DELETE FROM idempotency_records
WHERE scope = $1 AND idempotency_key = $2`, scope, key); err != nil {
			return nil, false, err
		}
		tag, err := tx.ExecContext(ctx, `INSERT INTO idempotency_records (scope, idempotency_key, request_hash, status, expires_at)
VALUES ($1, $2, $3, 'IN_PROGRESS', $4)`, scope, key, requestHash, s.clock().Add(idempotencyTTL))
		if err != nil {
			return nil, false, err
		}
		if affected, err := tag.RowsAffected(); err == nil && affected == 1 {
			return nil, false, nil
		}
		return nil, false, order.ErrIdempotencyInProgress
	}
	if storedHash != requestHash {
		return nil, false, order.ErrIdempotencyConflict
	}
	switch status {
	case "SUCCEEDED":
		return body, true, nil
	case "FAILED":
		// A failed attempt frees the key for an identical retry.
		if _, err := tx.ExecContext(ctx, `UPDATE idempotency_records SET status = 'IN_PROGRESS', updated_at = CURRENT_TIMESTAMP
WHERE scope = $1 AND idempotency_key = $2`, scope, key); err != nil {
			return nil, false, err
		}
		return nil, false, nil
	default:
		return nil, false, order.ErrIdempotencyInProgress
	}
}

func (s *OrderStore) finalizeIdempotency(tx *sql.Tx, ctx context.Context, scope, key string, body []byte) error {
	_, err := tx.ExecContext(ctx, `UPDATE idempotency_records
SET status = 'SUCCEEDED', response_code = $3, response_body = $4, updated_at = CURRENT_TIMESTAMP
WHERE scope = $1 AND idempotency_key = $2`, scope, key, 0, body)
	return err
}

// appendOutbox writes one event row inside the caller's transaction. The
// stream column stays empty: the B-line publisher derives the stream from the
// event type.
func (s *OrderStore) appendOutbox(tx *sql.Tx, ctx context.Context, eventType, aggregateID string, payload map[string]any, traceID string) error {
	eventID, err := order.NewEventID()
	if err != nil {
		return err
	}
	payloadBytes, err := json.Marshal(payload)
	if err != nil {
		return fmt.Errorf("marshal outbox payload: %w", err)
	}
	if _, err := tx.ExecContext(ctx, `INSERT INTO outbox_events (event_id, event_type, aggregate_type, aggregate_id, payload, trace_id)
VALUES ($1, $2, 'order', $3, $4::jsonb, $5)`,
		eventID, eventType, aggregateID, string(payloadBytes), traceID); err != nil {
		return fmt.Errorf("append outbox event: %w", err)
	}
	return nil
}

const orderSelectColumns = `id, order_no, user_id, station_id, charger_id, status, amount_cents, paid_cents, payment_status, energy_wh,
price_per_kwh_cents, service_price_per_kwh_cents, off_peak_price_per_kwh_cents, off_peak_start_hour, off_peak_end_hour,
started_at, created_at, updated_at`

type orderRow struct {
	ID int64 // internal surrogate key, never serialized
	order.Order
	PricePerKwhCents      int64 // electricity snapshot (peak), from the start transaction
	ServicePricePerKwhCen int64 // service fee snapshot
	OffPeakPrice          *int64
	OffPeakStartHour      *int16
	OffPeakEndHour        *int16
	StartedAt             sql.NullTime
}

func scanOrder(row *sql.Row) (orderRow, error) {
	var result orderRow
	var offPeakPrice sql.NullInt64
	var offPeakStartHour, offPeakEndHour sql.NullInt16
	err := row.Scan(&result.ID, &result.OrderNo, &result.UserID, &result.StationID, &result.ChargerID,
		&result.Status, &result.AmountCent, &result.PaidCent, &result.PaymentStatus, &result.EnergyWh,
		&result.PricePerKwhCents, &result.ServicePricePerKwhCen, &offPeakPrice, &offPeakStartHour, &offPeakEndHour,
		&result.StartedAt, &result.CreatedAt, &result.UpdatedAt)
	if err != nil {
		return orderRow{}, err
	}
	if offPeakPrice.Valid {
		value := offPeakPrice.Int64
		result.OffPeakPrice = &value
	}
	if offPeakStartHour.Valid {
		value := offPeakStartHour.Int16
		result.OffPeakStartHour = &value
	}
	if offPeakEndHour.Valid {
		value := offPeakEndHour.Int16
		result.OffPeakEndHour = &value
	}
	// TIMESTAMPTZ values arrive in the session zone; the API contract speaks UTC.
	result.CreatedAt = result.CreatedAt.UTC()
	result.UpdatedAt = result.UpdatedAt.UTC()
	return result, nil
}

// CreateOrder claims the idempotency slot, validates charger/station/balance/
// flow rules under a row lock, creates the order, occupies the charger and
// appends ORDER_CREATED — all in one transaction.
func (s *OrderStore) CreateOrder(ctx context.Context, command order.CreateOrderCommand) (order.Order, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return order.Order{}, err
	}
	defer func() { _ = tx.Rollback() }()

	scope := idempotencyScope("order:create", command.UserID, "")
	replay, replayed, err := s.claimIdempotency(tx, ctx, scope, command.IdempotencyKey, command.RequestHash)
	if err != nil {
		return order.Order{}, err
	}
	if replayed {
		return decodeReplayedOrder(replay)
	}

	var chargerID, stationID, priceCents int64
	var chargerStatus, stationStatus string
	err = tx.QueryRowContext(ctx, `SELECT c.id, c.station_id, c.status, c.price_per_kwh_cents, s.status
FROM chargers c
JOIN stations s ON s.id = c.station_id
WHERE c.id = $1
FOR UPDATE OF c`, command.ChargerID).Scan(&chargerID, &stationID, &chargerStatus, &priceCents, &stationStatus)
	if errors.Is(err, sql.ErrNoRows) {
		return order.Order{}, order.ErrChargerUnavailable
	}
	if err != nil {
		return order.Order{}, err
	}
	if chargerStatus != "IDLE" || stationStatus != "OPEN" {
		return order.Order{}, order.ErrChargerUnavailable
	}

	var balance int64
	err = tx.QueryRowContext(ctx, `SELECT balance_cents FROM wallet_accounts WHERE user_id = $1`, command.UserID).Scan(&balance)
	if errors.Is(err, sql.ErrNoRows) {
		balance = 0
	} else if err != nil {
		return order.Order{}, err
	}
	if balance < order.MinStartBalanceCents {
		return order.Order{}, order.ErrInsufficientBalance
	}

	var activeFlows int64
	if err := tx.QueryRowContext(ctx, `SELECT count(*) FROM charging_orders
WHERE user_id = $1 AND status IN `+activeStatusSQL, command.UserID).Scan(&activeFlows); err != nil {
		return order.Order{}, err
	}
	if activeFlows > 0 {
		return order.Order{}, order.ErrActiveFlowExists
	}

	// At most one unsettled order per user: a completed-but-unpaid order
	// blocks new flows until it is settled (UC-U-09).
	var unpaid int64
	if err := tx.QueryRowContext(ctx, `SELECT count(*) FROM charging_orders
WHERE user_id = $1 AND status = 'COMPLETED' AND payment_status <> 'PAID'`, command.UserID).Scan(&unpaid); err != nil {
		return order.Order{}, err
	}
	if unpaid > 0 {
		return order.Order{}, order.ErrDebtOutstanding
	}

	now := s.clock()
	orderNo, err := order.NewOrderNo(now)
	if err != nil {
		return order.Order{}, err
	}
	// The price snapshot happens at START (frozen requirement), not here.
	if _, err := tx.ExecContext(ctx, `INSERT INTO charging_orders (order_no, user_id, station_id, charger_id, status, requested_at)
VALUES ($1, $2, $3, $4, 'CREATED', $5)`,
		orderNo, command.UserID, stationID, chargerID, now); err != nil {
		// The in-transaction flow check can race with a concurrent create by
		// the same user; uq_orders_user_active is the final guard.
		if strings.Contains(err.Error(), "uq_orders_user_active") {
			return order.Order{}, order.ErrActiveFlowExists
		}
		return order.Order{}, err
	}

	if _, err := tx.ExecContext(ctx, `UPDATE chargers SET status = 'OCCUPIED', updated_at = CURRENT_TIMESTAMP WHERE id = $1`, chargerID); err != nil {
		return order.Order{}, err
	}

	result := order.Order{
		OrderNo:    orderNo,
		UserID:     command.UserID,
		StationID:  stationID,
		ChargerID:  chargerID,
		Status:     order.StatusCreated,
		AmountCent: 0,
		CreatedAt:  now.UTC(),
		UpdatedAt:  now.UTC(),
	}
	if err := s.appendOutbox(tx, ctx, order.EventOrderCreated, orderNo, map[string]any{
		"orderNo":   orderNo,
		"userId":    command.UserID,
		"stationId": stationID,
		"chargerId": chargerID,
	}, command.TraceID); err != nil {
		return order.Order{}, err
	}

	body, err := json.Marshal(result)
	if err != nil {
		return order.Order{}, err
	}
	if err := s.finalizeIdempotency(tx, ctx, scope, command.IdempotencyKey, body); err != nil {
		return order.Order{}, err
	}

	if err := tx.Commit(); err != nil {
		return order.Order{}, err
	}
	return result, nil
}

// StartCharging moves CREATED → STARTING and appends CHARGE_START_REQUESTED.
func (s *OrderStore) StartCharging(ctx context.Context, command order.TransitionCommand) (order.Order, error) {
	return s.transitionOrder(ctx, command, "order:start", order.StatusStarting,
		order.EventChargeStartRequested, order.CommandStartCharging)
}

// StopCharging moves CHARGING → STOPPING and appends CHARGE_STOP_REQUESTED.
func (s *OrderStore) StopCharging(ctx context.Context, command order.TransitionCommand) (order.Order, error) {
	return s.transitionOrder(ctx, command, "order:stop", order.StatusStopping,
		order.EventChargeStopRequested, order.CommandStopCharging)
}

// CancelOrder moves a CREATED or STARTING order to CANCELLED and releases
// the charger in the same transaction. Idempotent through the shared
// idempotency-key mechanism (UC-U-07).
func (s *OrderStore) CancelOrder(ctx context.Context, command order.TransitionCommand) (order.Order, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return order.Order{}, err
	}
	defer func() { _ = tx.Rollback() }()

	scope := idempotencyScope("order:cancel", command.UserID, command.OrderNo)
	replay, replayed, err := s.claimIdempotency(tx, ctx, scope, command.IdempotencyKey, command.RequestHash)
	if err != nil {
		return order.Order{}, err
	}
	if replayed {
		return decodeReplayedOrder(replay)
	}

	row, err := s.lockOrder(tx, ctx, command.OrderNo, command.UserID)
	if err != nil {
		return order.Order{}, err
	}
	result := row.Order
	if !order.CanTransition(result.Status, order.StatusCancelled) {
		return order.Order{}, fmt.Errorf("%w: %s -> %s", order.ErrInvalidStateTransition, result.Status, order.StatusCancelled)
	}

	if _, err := tx.ExecContext(ctx, `UPDATE charging_orders
SET status = 'CANCELLED', version = version + 1, updated_at = CURRENT_TIMESTAMP
WHERE id = $1`, row.ID); err != nil {
		return order.Order{}, err
	}
	if result.Status == order.StatusStarting {
		// The start command already reached the device, so the charger is
		// NOT released here: it stays occupied until the revocation command
		// below has been issued and the orphan sweep observes the order is
		// terminal. Handing the device to the next user now could let the
		// physical charge continue onto someone else's order.
		if err := s.appendChargerCommand(tx, ctx, result.ChargerID, "cancel_start"); err != nil {
			return order.Order{}, err
		}
	} else {
		// A CREATED order never reached the device: release immediately.
		if _, err := tx.ExecContext(ctx, `UPDATE chargers SET status = 'IDLE', updated_at = CURRENT_TIMESTAMP
WHERE id = $1 AND status = 'OCCUPIED'`, result.ChargerID); err != nil {
			return order.Order{}, err
		}
	}
	result.Status = order.StatusCancelled
	result.UpdatedAt = s.clock().UTC()

	body, err := json.Marshal(result)
	if err != nil {
		return order.Order{}, err
	}
	if err := s.finalizeIdempotency(tx, ctx, scope, command.IdempotencyKey, body); err != nil {
		return order.Order{}, err
	}
	if err := tx.Commit(); err != nil {
		return order.Order{}, err
	}
	return result, nil
}

// appendChargerCommand writes a device command event to the outbox so the
// B-line worker revokes a previously issued start command. The payload must
// match the B-04 consumer contract exactly — command_id, charger_id and
// action — and RESTART is the only action the first phase defines: a device
// restart is what aborts a pending start on the physical charger.
func (s *OrderStore) appendChargerCommand(tx *sql.Tx, ctx context.Context, chargerID int64, reason string) error {
	commandID, err := order.NewEventID()
	if err != nil {
		return err
	}
	return s.appendOutbox(tx, ctx, order.EventChargerCommandRequested, strconv.FormatInt(chargerID, 10),
		map[string]any{"command_id": commandID, "charger_id": strconv.FormatInt(chargerID, 10), "action": "RESTART", "reason": reason}, "")
}

// ExpireStaleOrders releases chargers held by abandoned orders: CREATED
// orders age to EXPIRED, stuck STARTING orders to FAILED. Rows are locked
// with SKIP LOCKED so several API instances can sweep concurrently.
func (s *OrderStore) ExpireStaleOrders(ctx context.Context, olderThan time.Duration) (int, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return 0, err
	}
	defer func() { _ = tx.Rollback() }()

	cutoff := s.clock().Add(-olderThan)
	rows, err := tx.QueryContext(ctx, `SELECT id, charger_id, status, start_requested_at FROM charging_orders
WHERE (status = 'CREATED' AND requested_at < $1)
   OR (status = 'STARTING' AND start_requested_at < $1)
ORDER BY id
FOR UPDATE SKIP LOCKED`, cutoff)
	if err != nil {
		return 0, err
	}
	type stale struct {
		id      int64
		charger int64
		created bool
	}
	var staleOrders []stale
	for rows.Next() {
		var row stale
		var status string
		var startRequested sql.NullTime
		if err := rows.Scan(&row.id, &row.charger, &status, &startRequested); err != nil {
			rows.Close()
			return 0, err
		}
		row.created = status == "CREATED"
		staleOrders = append(staleOrders, row)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return 0, err
	}
	rows.Close()

	for _, row := range staleOrders {
		target := "FAILED"
		if row.created {
			target = "EXPIRED"
		}
		if _, err := tx.ExecContext(ctx, `UPDATE charging_orders
SET status = $2, version = version + 1, updated_at = CURRENT_TIMESTAMP
WHERE id = $1`, row.id, target); err != nil {
			return 0, err
		}
		if row.created {
			// Never reached the device: release immediately.
			if _, err := tx.ExecContext(ctx, `UPDATE chargers SET status = 'IDLE', updated_at = CURRENT_TIMESTAMP
WHERE id = $1 AND status = 'OCCUPIED'`, row.charger); err != nil {
				return 0, err
			}
		} else {
			// The start command already reached the device: keep it occupied
			// and queue the revocation; the orphan sweep releases the
			// charger once the order is terminal.
			if err := s.appendChargerCommand(tx, ctx, row.charger, "expire_stale_start"); err != nil {
				return 0, err
			}
		}
	}

	if err := tx.Commit(); err != nil {
		return 0, err
	}
	return len(staleOrders), nil
}

// SettleOrder executes the UC-U-09 user confirmation: the pending bill is
// deducted from the wallet (down to zero on an insufficient balance), a
// CHARGE ledger row records the movement, and the payment state becomes
// PAID or PARTIAL_PAID. Idempotent through the shared key mechanism.
func (s *OrderStore) SettleOrder(ctx context.Context, command order.SettleCommand) (order.Order, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return order.Order{}, err
	}
	defer func() { _ = tx.Rollback() }()

	scope := idempotencyScope("order:confirm", command.UserID, command.OrderNo)
	replay, replayed, err := s.claimIdempotency(tx, ctx, scope, command.IdempotencyKey, command.RequestHash)
	if err != nil {
		return order.Order{}, err
	}
	if replayed {
		return decodeReplayedOrder(replay)
	}

	row, err := s.lockOrder(tx, ctx, command.OrderNo, command.UserID)
	if err != nil {
		return order.Order{}, err
	}
	result := row.Order
	if result.Status != order.StatusCompleted {
		return order.Order{}, fmt.Errorf("%w: settle requires a completed order, order is %s", order.ErrInvalidStateTransition, result.Status)
	}
	if result.PaymentStatus == "PAID" {
		return order.Order{}, fmt.Errorf("%w: order is already settled", order.ErrInvalidStateTransition)
	}

	remaining := result.AmountCent - result.PaidCent
	var balance int64
	err = tx.QueryRowContext(ctx, `SELECT balance_cents FROM wallet_accounts WHERE user_id = $1 FOR UPDATE`, result.UserID).Scan(&balance)
	if errors.Is(err, sql.ErrNoRows) {
		if _, err := tx.ExecContext(ctx, `INSERT INTO wallet_accounts (user_id, balance_cents) VALUES ($1, 0) ON CONFLICT DO NOTHING`, result.UserID); err != nil {
			return order.Order{}, err
		}
		balance = 0
	} else if err != nil {
		return order.Order{}, err
	}

	paid := remaining
	if paid > balance {
		paid = balance // BR-06: an insufficient balance is drawn to zero, the remainder stays recorded as debt on the order
	}
	paymentStatus := "PARTIAL_PAID"
	if paid == remaining {
		paymentStatus = "PAID"
	}

	if paid > 0 {
		if _, err := tx.ExecContext(ctx, `UPDATE wallet_accounts
SET balance_cents = balance_cents - $2, version = version + 1, updated_at = CURRENT_TIMESTAMP
WHERE user_id = $1`, result.UserID, paid); err != nil {
			return order.Order{}, err
		}
		if _, err := tx.ExecContext(ctx, `INSERT INTO wallet_transactions
    (user_id, order_id, transaction_type, amount_cents, balance_before_cents, balance_after_cents, idempotency_key)
VALUES ($1, $2, 'CHARGE', $3, $4, $5, $6)`,
			result.UserID, row.ID, -paid, balance, balance-paid, "order:"+result.OrderNo+":confirm"); err != nil {
			return order.Order{}, err
		}
	}

	if _, err := tx.ExecContext(ctx, `UPDATE charging_orders
SET paid_cents = $2, payment_status = $3, version = version + 1, updated_at = CURRENT_TIMESTAMP
WHERE id = $1`, row.ID, result.PaidCent+paid, paymentStatus); err != nil {
		return order.Order{}, err
	}
	result.PaidCent += paid
	result.PaymentStatus = paymentStatus
	result.UpdatedAt = s.clock().UTC()

	body, err := json.Marshal(result)
	if err != nil {
		return order.Order{}, err
	}
	if err := s.finalizeIdempotency(tx, ctx, scope, command.IdempotencyKey, body); err != nil {
		return order.Order{}, err
	}
	if err := tx.Commit(); err != nil {
		return order.Order{}, err
	}
	return result, nil
}

// ReleaseOrphanedChargers frees OCCUPIED chargers that no longer have an
// active order and never sent a start command (CREATED-path cancels and
// expiries). Chargers whose start command already reached the device keep
// waiting for the revocation confirmation via CompleteChargerCommand —
// releasing them here would hand a possibly still-charging device to the
// next user.
func (s *OrderStore) ReleaseOrphanedChargers(ctx context.Context) (int, error) {
	tag, err := s.db.ExecContext(ctx, `UPDATE chargers c
SET status = 'IDLE', updated_at = CURRENT_TIMESTAMP
WHERE c.status = 'OCCUPIED'
  AND NOT EXISTS (
      SELECT 1 FROM charging_orders o
      WHERE o.charger_id = c.id AND o.status IN `+activeStatusSQL+`)
  AND NOT EXISTS (
      SELECT 1 FROM charging_orders o
      WHERE o.charger_id = c.id AND o.start_requested_at IS NOT NULL)`)
	if err != nil {
		return 0, err
	}
	affected, err := tag.RowsAffected()
	if err != nil {
		return 0, err
	}
	return int(affected), nil
}

// CompleteChargerCommand applies the device-side outcome of a charger
// command (the B-line worker calls it when CHARGER_COMMAND_COMPLETED
// arrives). The outcome decides the status: COMPLETED returns the charger
// to IDLE, anything else parks it in FAULT because the device's health is
// unknown. It only moves a charger the command was holding, and only while
// no active order holds it, so a late device success can never hand a
// charging device to a new order.
func (s *OrderStore) CompleteChargerCommand(ctx context.Context, chargerID int64, action, result string) (bool, error) {
	return completeChargerCommandTx(s.db, ctx, chargerID, action, result)
}

// RecordChargerCommandResult applies the device-side outcome of a charger command and appends
// the CHARGER_COMMAND_COMPLETED event in the same transaction.
//
// The event has to be produced with the state change it describes: emitting it separately would
// let a consumer observe an outcome the database never applied, or a state change no consumer
// is ever told about. This is the entry point the B-line worker's dispatcher calls after the
// gateway answers, which is what closes "command -> device -> completion -> order state".
//
// The frozen failure semantics (BE-I-02) live here, because this is the only place that knows both
// the device outcome and the order it belongs to:
//
//   - a command the device merely accepted advances no order status; the receipt does;
//   - an explicit START_CHARGING failure moves its order to FAILED and parks the charger in FAULT,
//     so a device that refused to start does not leave an order waiting forever in STARTING;
//   - an explicit STOP_CHARGING failure keeps the order in STOPPING - the platform must not assume
//     a device stopped when it says it did not - and parks the charger in FAULT for the recovery
//     sweep to retry with a new command id;
//   - the recovery path never releases the charger and never settles the order.
//
// The completion event is emitted exactly when something was applied. A charge command the device
// accepted changes no row, so it produces no event: there is nothing to complete, and the receipt
// that follows is what carries the next fact.
//
// One command id has exactly one outcome, and that is enforced inside this transaction, in a record
// that does not expire. Without the claim a replay was indistinguishable from a first application
// whenever the outcome changed no state - which is exactly the STOP_CHARGING failure case, where the
// order deliberately stays STOPPING: the replay re-decided "applied", wrote a second
// CHARGER_COMMAND_COMPLETED and told every consumer about the same device verdict twice.
//
// The record is charger_command_outcomes, not an idempotency_records row. An idempotency record is a
// replay cache with a 24-hour window, which is right for a client retry and wrong for a device
// verdict: an order in STOPPING with a faulty charger is still meaningful a day later - it is the
// state the STOP recovery sweep works on - so an expiring record let a duplicate STOP result be
// applied again long after the first one.
func (s *OrderStore) RecordChargerCommandResult(ctx context.Context, commandNo, orderNo string, chargerID int64, action, result, traceID string) (bool, error) {
	if chargerID < 1 || strings.TrimSpace(commandNo) == "" {
		return false, nil
	}
	commandNo = strings.TrimSpace(commandNo)
	orderNo = strings.TrimSpace(orderNo)
	action = strings.ToUpper(strings.TrimSpace(action))
	result = strings.ToUpper(strings.TrimSpace(result))
	if !order.IsCommandResult(result) {
		// Only the frozen enum may be applied. Anything else - a timeout, a status this platform
		// does not know - means the gateway has not told us what the device did, and treating it as
		// a failure would fail an order or park a charger on a guess. The table refuses it as well.
		return false, fmt.Errorf("postgres: %q is not a device command outcome for command %s", result, commandNo)
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return false, err
	}
	defer func() { _ = tx.Rollback() }()

	recorded, err := s.claimCommandOutcome(tx, ctx, commandOutcome{
		commandID: commandNo, chargerID: chargerID, orderNo: orderNo, action: action, result: result,
	})
	if err != nil {
		return false, err
	}
	if recorded {
		// The outcome was already recorded. Nothing is applied and no event is emitted: the state
		// change and the event that describes it happened in the transaction that claimed this id.
		if err := tx.Commit(); err != nil {
			return false, err
		}
		return false, nil
	}

	var applied bool
	switch action {
	case order.CommandRestart:
		applied, err = completeChargerCommandTx(tx, ctx, chargerID, action, result)
	case order.CommandStartCharging, order.CommandStopCharging:
		applied, err = s.applyChargeCommandOutcome(tx, ctx, orderNo, chargerID, action, result)
	default:
		// An action outside the frozen set never reached a device through this
		// platform, so there is no outcome to apply.
		applied = false
	}
	if err != nil {
		return false, err
	}
	if applied {
		// The payload matches the B-04 consumer contract for a charger command: the same three
		// fields the request carries, plus the device outcome.
		payload := map[string]any{
			"command_id": commandNo,
			"charger_id": strconvFormatInt64(chargerID),
			"action":     action,
			"result":     result,
		}
		if err := s.appendOutbox(tx, ctx, order.EventChargerCommandCompleted, strconvFormatInt64(chargerID), payload, traceID); err != nil {
			return false, err
		}
	}
	// The outcome is recorded even when it changed nothing: the command has been answered, and a
	// later delivery must be recognised as the same verdict rather than evaluated again.
	if _, err := tx.ExecContext(ctx, `UPDATE charger_command_outcomes SET applied = $2 WHERE command_id = $1`,
		commandNo, applied); err != nil {
		return false, err
	}
	if err := tx.Commit(); err != nil {
		return false, err
	}
	return applied, nil
}

// commandOutcome identifies one device verdict: which command, which device, which order, which
// action and which result. A row that disagrees with any of them is a different command wearing a
// used id, not a repeat.
type commandOutcome struct {
	commandID string
	chargerID int64
	orderNo   string
	action    string
	result    string
}

// claimCommandOutcome claims the command's outcome row inside the caller's transaction and reports
// whether the command already had an outcome.
//
// The insert decides the race: the primary key on the command id means exactly one delivery can
// claim it, and a concurrent delivery blocks on the conflict and then reads the committed row, so it
// comes back as a replay instead of applying the same verdict a second time. A stored row that
// differs in any field is a contradiction: the device answered once, and the platform must not
// rewrite what it said.
func (s *OrderStore) claimCommandOutcome(tx *sql.Tx, ctx context.Context, outcome commandOutcome) (bool, error) {
	tag, err := tx.ExecContext(ctx, `INSERT INTO charger_command_outcomes
    (command_id, charger_id, order_no, action, result, applied)
VALUES ($1, $2, $3, $4, $5, false)
ON CONFLICT (command_id) DO NOTHING`,
		outcome.commandID, outcome.chargerID, outcome.orderNo, outcome.action, outcome.result)
	if err != nil {
		return false, err
	}
	if affected, err := tag.RowsAffected(); err == nil && affected == 1 {
		return false, nil
	}

	var stored commandOutcome
	if err := tx.QueryRowContext(ctx, `SELECT command_id, charger_id, order_no, action, result
   FROM charger_command_outcomes
  WHERE command_id = $1`, outcome.commandID).
		Scan(&stored.commandID, &stored.chargerID, &stored.orderNo, &stored.action, &stored.result); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			// The conflicting row vanished with its transaction; the caller retries and claims it.
			return false, order.ErrIdempotencyInProgress
		}
		return false, err
	}
	if stored.chargerID != outcome.chargerID || stored.orderNo != outcome.orderNo ||
		stored.action != outcome.action || stored.result != outcome.result {
		return false, order.ErrIdempotencyConflict
	}
	return true, nil
}

// applyChargeCommandOutcome applies the frozen outcome of a charge command inside the caller's
// transaction and reports whether anything changed.
//
// A successful command changes nothing: the device accepting an instruction is not the physical
// fact, and only the receipt may advance an order. A failure is a physical fact the platform must
// act on, and what it acts on depends on the action: a start that failed ends the order, while a
// stop that failed leaves the order STOPPING with a charger in FAULT - the device may still be
// charging, so nothing may be released or settled here.
func (s *OrderStore) applyChargeCommandOutcome(tx *sql.Tx, ctx context.Context, orderNo string, chargerID int64, action, result string) (bool, error) {
	if strings.EqualFold(strings.TrimSpace(result), order.CommandResultCompleted) {
		// The device accepted the command. That is not the physical fact and it
		// advances nothing: the receipt reports what actually happened.
		return false, nil
	}
	// Only an explicit refusal is left: the caller has already refused every value
	// outside the frozen outcome enum.
	if !strings.EqualFold(strings.TrimSpace(result), order.CommandResultFailed) {
		return false, nil
	}
	// A failure needs the order it belongs to. Without one there is nothing to
	// move, and guessing from the charger would risk failing the wrong order.
	if orderNo == "" {
		return false, nil
	}
	row, err := s.lockOrder(tx, ctx, orderNo, 0)
	if err != nil {
		if errors.Is(err, order.ErrOrderNotFound) {
			return false, nil
		}
		return false, err
	}
	if row.ChargerID != chargerID {
		// The outcome names a different device than the order holds, so
		// attributing it would move the wrong order. Nothing is applied.
		return false, nil
	}

	switch {
	case action == order.CommandStartCharging && row.Status == order.StatusStarting:
		// The device refused to start: the order ends here rather than waiting
		// for a receipt that will never come.
		if _, err := tx.ExecContext(ctx, `UPDATE charging_orders
SET status = 'FAILED', version = version + 1, updated_at = CURRENT_TIMESTAMP
WHERE id = $1`, row.ID); err != nil {
			return false, err
		}
	case action == order.CommandStopCharging && row.Status == order.StatusStopping:
		// Deliberately no order update: the platform cannot assume the device
		// stopped, and settling now would bill a charge that may still be
		// running. The recovery sweep owns this order from here.
	default:
		// The order already moved past this command - a receipt got there first,
		// or the order is terminal. The outcome is stale, and a stale failure
		// must not fail a running charge or park a device whose confirmation was
		// already accepted.
		return false, nil
	}

	// A failed charge command leaves a device whose state nobody can vouch for,
	// so it goes to FAULT in both cases: for a failed start, and - more
	// importantly - for a failed stop, where the order deliberately stays
	// STOPPING until the recovery sweep gets an explicit stop out of the device.
	if _, err := tx.ExecContext(ctx, `UPDATE chargers SET status = 'FAULT', updated_at = CURRENT_TIMESTAMP
WHERE id = $1 AND status <> 'FAULT'`, chargerID); err != nil {
		return false, err
	}
	return true, nil
}

// ReissueStopCommands is the STOP recovery sweep.
//
// An order stuck in STOPPING with a charger in FAULT is the state a failed STOP leaves behind, and
// it is a state that must not be resolved by assumption: the device may still be charging, so
// nothing here releases the charger or settles the order. What it does instead is re-send
// STOP_CHARGING with a NEW command id (a reused id would be answered from the gateway's stored
// outcome, so the device would never see the retry), bounded by the policy and spaced by its
// backoff, and after the last attempt it hands the order to an operator by writing a one-off
// escalation row.
//
// The attempt count comes from the commands already recorded for the order - the outbox rows are
// the audit trail of what was sent - so the sweep needs no new table and cannot lose count across
// restarts. Orders are locked with SKIP LOCKED, so several API replicas can run the sweep without
// double-issuing a command.
func (s *OrderStore) ReissueStopCommands(ctx context.Context, policy order.StopRecoveryPolicy) (order.StopRecoveryResult, error) {
	result := order.StopRecoveryResult{}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return result, err
	}
	defer func() { _ = tx.Rollback() }()

	rows, err := tx.QueryContext(ctx, `SELECT o.id, o.order_no, o.charger_id, o.status, o.user_id
   FROM charging_orders o
   JOIN chargers c ON c.id = o.charger_id
  WHERE o.status = 'STOPPING' AND c.status = 'FAULT'
  ORDER BY o.id
  LIMIT 50
    FOR UPDATE OF o SKIP LOCKED`)
	if err != nil {
		return result, err
	}
	type stuckOrder struct {
		id        int64
		orderNo   string
		chargerID int64
		userID    int64
	}
	var stuck []stuckOrder
	for rows.Next() {
		var candidate stuckOrder
		var status string
		if err := rows.Scan(&candidate.id, &candidate.orderNo, &candidate.chargerID, &status, &candidate.userID); err != nil {
			_ = rows.Close()
			return result, err
		}
		stuck = append(stuck, candidate)
	}
	if err := rows.Err(); err != nil {
		_ = rows.Close()
		return result, err
	}
	_ = rows.Close()

	now := s.clock()
	for _, candidate := range stuck {
		var attempts int
		var newest sql.NullTime
		if err := tx.QueryRowContext(ctx, `SELECT count(*), max(created_at)
   FROM outbox_events
  WHERE event_type = $1 AND payload->>'order_no' = $2 AND payload->>'action' = $3`,
			order.EventChargerCommandRequested, candidate.orderNo, order.CommandStopCharging).
			Scan(&attempts, &newest); err != nil {
			return result, err
		}

		if attempts >= policy.MaxAttempts {
			// Out of attempts: keep STOPPING and FAULT, and tell a human once.
			escalated, err := s.escalateStopRecovery(tx, ctx, candidate.id, candidate.orderNo, candidate.chargerID, attempts)
			if err != nil {
				return result, err
			}
			if escalated {
				result.Escalated++
			}
			continue
		}
		if newest.Valid && now.Sub(newest.Time) < policy.Backoff {
			result.Waiting++
			continue
		}

		commandNo, err := admin.NewCommandID(now)
		if err != nil {
			return result, err
		}
		payload := map[string]any{
			"command_id": commandNo,
			"charger_id": strconvFormatInt64(candidate.chargerID),
			"order_no":   candidate.orderNo,
			"action":     order.CommandStopCharging,
			"recovery":   true,
		}
		if err := s.appendOutbox(tx, ctx, order.EventChargerCommandRequested,
			strconvFormatInt64(candidate.chargerID), payload, "stop-recovery"); err != nil {
			return result, err
		}
		result.Reissued++
	}

	if err := tx.Commit(); err != nil {
		return order.StopRecoveryResult{}, err
	}
	return result, nil
}

// escalateStopRecovery writes the one-off hand-over to an operator and reports whether this sweep
// is the one that wrote it. The row is the durable marker: without it every sweep would alert again
// about the same order, and "waiting for a human" would drown in its own repetition.
func (s *OrderStore) escalateStopRecovery(tx *sql.Tx, ctx context.Context, orderID int64, orderNo string, chargerID int64, attempts int) (bool, error) {
	var exists bool
	if err := tx.QueryRowContext(ctx, `SELECT EXISTS (
    SELECT 1 FROM order_events WHERE order_id = $1 AND event_type = 'STOP_RECOVERY_ESCALATED'
)`, orderID).Scan(&exists); err != nil {
		return false, err
	}
	if exists {
		return false, nil
	}
	escalationID, err := order.NewEventID()
	if err != nil {
		return false, err
	}
	payload, err := json.Marshal(map[string]any{
		"orderNo":           orderNo,
		"chargerId":         strconvFormatInt64(chargerID),
		"stopAttempts":      attempts,
		"action":            "manual_intervention",
		"orderStatus":       order.StatusStopping,
		"chargerStatus":     "FAULT",
		"automaticRelease":  false,
		"automaticSettle":   false,
		"escalatedBy":       "stop-recovery-sweep",
		"escalatedAtServer": s.clock().UTC().Format(time.RFC3339),
	})
	if err != nil {
		return false, err
	}
	if _, err := tx.ExecContext(ctx, `INSERT INTO order_events (order_id, event_id, event_type, payload)
VALUES ($1, $2, 'STOP_RECOVERY_ESCALATED', $3::jsonb)`, orderID, escalationID, string(payload)); err != nil {
		return false, err
	}
	return true, nil
}

// chargerCommandStatus maps a device outcome to the status the charger must end up in.
//
// A restart that COMPLETED leaves a usable charger, so it returns to IDLE and can be allocated.
// Everything else - FAILED, TIMED_OUT, an outcome the gateway invented - leaves a charger whose
// state is unknown or broken, so it goes to FAULT and must not be handed to a new order. Treating
// those as IDLE was a real defect: the reviewer forced the mock gateway to answer FAILED and
// observed the charger being released as IDLE, which would put a possibly broken device back into
// the allocation pool.
//
// There is no "restore the previous status" branch because the previous status is not recorded
// anywhere: the admin restart overwrites it. Reconstructing it from the order table would be a
// guess, and a guess about a device's health is worse than an explicit FAULT that an operator
// clears.
func chargerCommandStatus(result string) string {
	if strings.EqualFold(strings.TrimSpace(result), order.CommandResultCompleted) {
		return "IDLE"
	}
	return "FAULT"
}

// appendOrderChargerCommand queues the device command that belongs to an order transition, inside
// the caller's transaction.
//
// The payload matches the frozen command contract: command_id, charger_id, action and order_no.
// order_no is what lets the gateway - and the receipt it sends back - name the order a command
// belongs to; the station-level RESTART compensation below carries none.
func (s *OrderStore) appendOrderChargerCommand(tx *sql.Tx, ctx context.Context, o order.Order, action, traceID string) error {
	commandNo, err := admin.NewCommandID(s.clock())
	if err != nil {
		return err
	}
	payload := map[string]any{
		"command_id": commandNo,
		"charger_id": strconvFormatInt64(o.ChargerID),
		"order_no":   o.OrderNo,
		"action":     action,
	}
	return s.appendOutbox(tx, ctx, order.EventChargerCommandRequested, strconvFormatInt64(o.ChargerID), payload, traceID)
}

// completeChargerCommandTx applies a charger command outcome to the charger row, but only while
// no active order holds the charger, so a late device success can never hand a charging device to
// a new order.
func completeChargerCommandTx(tx interface {
	ExecContext(context.Context, string, ...any) (sql.Result, error)
}, ctx context.Context, chargerID int64, action, result string) (bool, error) {
	if strings.ToUpper(strings.TrimSpace(action)) != "RESTART" {
		// The first device-command phase defines RESTART only; unknown actions never reach the
		// device and have nothing to complete.
		return false, nil
	}
	tag, err := tx.ExecContext(ctx, `UPDATE chargers c
SET status = $2, updated_at = CURRENT_TIMESTAMP
WHERE c.id = $1 AND c.status IN ('OCCUPIED', 'RESTARTING')
  AND NOT EXISTS (
      SELECT 1 FROM charging_orders o
      WHERE o.charger_id = c.id AND o.status IN `+activeStatusSQL+`)`, chargerID, chargerCommandStatus(result))
	if err != nil {
		return false, err
	}
	affected, err := tag.RowsAffected()
	if err != nil {
		return false, err
	}
	return affected > 0, nil
}

// transitionOrder applies one user-driven order transition.
//
// deviceAction is the charger command this transition has to put on the wire, or empty when the
// transition is not a device action (cancel). The command event is appended in the same
// transaction as the state change, which is what makes "the order moved" and "a command was
// queued" the same fact: a state change nobody is told about, or a command for a state the
// database never reached, are both worse than a retry. Extending the action set alone would
// produce nothing - the order transaction is what produces the command.
func (s *OrderStore) transitionOrder(ctx context.Context, command order.TransitionCommand, operation, targetStatus, eventType, deviceAction string) (order.Order, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return order.Order{}, err
	}
	defer func() { _ = tx.Rollback() }()

	scope := idempotencyScope(operation, command.UserID, command.OrderNo)
	replay, replayed, err := s.claimIdempotency(tx, ctx, scope, command.IdempotencyKey, command.RequestHash)
	if err != nil {
		return order.Order{}, err
	}
	if replayed {
		return decodeReplayedOrder(replay)
	}

	row, err := s.lockOrder(tx, ctx, command.OrderNo, command.UserID)
	if err != nil {
		return order.Order{}, err
	}
	result := row.Order
	if err := order.ValidateTransition(result.Status, targetStatus); err != nil {
		return order.Order{}, err
	}
	if targetStatus == order.StatusStarting {
		// BR-04 applies to starting as much as to creating: the account must
		// still be active and funded at the moment the charge begins.
		if err := s.validateStartEligibility(tx, ctx, result.UserID); err != nil {
			return order.Order{}, err
		}
		if err := s.snapshotTariff(tx, ctx, row, s.clock()); err != nil {
			return order.Order{}, err
		}
	}

	if _, err := tx.ExecContext(ctx, `UPDATE charging_orders
SET status = $2, version = version + 1, updated_at = CURRENT_TIMESTAMP
WHERE id = $1`, row.ID, targetStatus); err != nil {
		return order.Order{}, err
	}
	result.Status = targetStatus
	result.UpdatedAt = s.clock().UTC()

	if err := s.appendOutbox(tx, ctx, eventType, result.OrderNo, map[string]any{
		"orderNo":   result.OrderNo,
		"userId":    result.UserID,
		"stationId": result.StationID,
		"chargerId": result.ChargerID,
	}, command.TraceID); err != nil {
		return order.Order{}, err
	}

	// The device command travels with the lifecycle notification, not instead of it: the first
	// says what happened to the order, the second is the instruction the charger has to receive.
	if deviceAction != "" {
		if err := s.appendOrderChargerCommand(tx, ctx, result, deviceAction, command.TraceID); err != nil {
			return order.Order{}, err
		}
	}

	body, err := json.Marshal(result)
	if err != nil {
		return order.Order{}, err
	}
	if err := s.finalizeIdempotency(tx, ctx, scope, command.IdempotencyKey, body); err != nil {
		return order.Order{}, err
	}

	if err := tx.Commit(); err != nil {
		return order.Order{}, err
	}
	return result, nil
}

// snapshotTariff captures the full tariff at the moment charging starts
// (frozen requirement): peak electricity price, the off-peak window and the
// service fee, all under the charger row lock, so later price changes never
// rewrite this order's bill. The time-of-use settlement splits the charge
// across the window at completion.
func (s *OrderStore) snapshotTariff(tx *sql.Tx, ctx context.Context, row orderRow, now time.Time) error {
	var peakPrice, servicePrice int64
	var offPeak sql.NullInt64
	var offPeakStartHour, offPeakEndHour sql.NullInt16
	err := tx.QueryRowContext(ctx, `SELECT price_per_kwh_cents, service_price_per_kwh_cents, off_peak_electricity_price_per_kwh_cents,
       off_peak_start_hour, off_peak_end_hour
FROM chargers WHERE id = $1 FOR UPDATE`, row.ChargerID).Scan(
		&peakPrice, &servicePrice, &offPeak, &offPeakStartHour, &offPeakEndHour)
	if err != nil {
		return err
	}
	_, err = tx.ExecContext(ctx, `UPDATE charging_orders
SET price_per_kwh_cents = $2, service_price_per_kwh_cents = $3,
    off_peak_price_per_kwh_cents = $4, off_peak_start_hour = $5, off_peak_end_hour = $6,
    start_requested_at = $7
WHERE id = $1`,
		row.ID, peakPrice, servicePrice,
		sql.NullInt64{Int64: offPeak.Int64, Valid: offPeak.Valid},
		sql.NullInt16{Int16: offPeakStartHour.Int16, Valid: offPeakStartHour.Valid},
		sql.NullInt16{Int16: offPeakEndHour.Int16, Valid: offPeakEndHour.Valid},
		now)
	return err
}

// validateStartEligibility re-checks the account state inside the start
// transaction: ACTIVE status and at least the minimum start balance.
func (s *OrderStore) validateStartEligibility(tx *sql.Tx, ctx context.Context, userID int64) error {
	var status string
	var balance int64
	err := tx.QueryRowContext(ctx, `SELECT u.status, COALESCE(w.balance_cents, 0)
FROM user_accounts u
LEFT JOIN wallet_accounts w ON w.user_id = u.id
WHERE u.id = $1`, userID).Scan(&status, &balance)
	if errors.Is(err, sql.ErrNoRows) {
		return order.ErrUserFrozen
	}
	if err != nil {
		return err
	}
	if status != "ACTIVE" {
		return order.ErrUserFrozen
	}
	if balance < order.MinStartBalanceCents {
		return order.ErrInsufficientBalance
	}
	return nil
}

// receiptScope is the idempotency scope of the device receipt endpoint. The
// receipt id identifies the fact itself, so the scope is a constant instead of
// the per-user scope the user-driven transitions use.
const receiptScope = "charger-events"

// receiptClaim is the outcome of claiming a receipt's idempotency slot.
type receiptClaim struct {
	// claimed reports whether this transaction owns the slot and must finalize
	// or release it.
	claimed bool
	// done reports that the receipt was already applied and body is its first
	// result, which is what a duplicate delivery must be answered with.
	done bool
	body []byte
}

// claimReceipt claims the receipt id inside the transaction.
//
// The claim happens before the state machine runs, and that order is the point:
// a duplicate arrival after the first one committed would otherwise be refused
// as an illegal transition instead of being answered with the first result.
// Claiming and finalizing stay inside the same transaction as the state change,
// so a committed order can never exist without its applied receipt.
//
// This one keeps the bounded idempotency window (24h) because the state machine is the durable
// guard behind it: a receipt that is applied moves its order to CHARGING or COMPLETED, and a
// duplicate arriving after the window has expired is refused as an illegal transition (409) and
// audited rather than applied again. A device command outcome has no such guard - a STOP failure
// deliberately leaves its order in STOPPING - which is why that record is permanent instead
// (charger_command_outcomes, BE-I-02 review).
func (s *OrderStore) claimReceipt(tx *sql.Tx, ctx context.Context, eventID, requestHash string) (receiptClaim, error) {
	id := strings.TrimSpace(eventID)
	if id == "" {
		// A direct store caller (not the receipt endpoint) is not deduplicated;
		// the order row lock and the state machine still guard it.
		return receiptClaim{}, nil
	}
	if strings.TrimSpace(requestHash) == "" {
		return receiptClaim{}, fmt.Errorf("%w: the payload digest is required", order.ErrInvalidReceiptID)
	}
	body, done, err := s.claimIdempotency(tx, ctx, receiptScope, id, requestHash)
	if err != nil {
		return receiptClaim{}, err
	}
	if done {
		return receiptClaim{done: true, body: body}, nil
	}
	return receiptClaim{claimed: true}, nil
}

// receiptAudit describes a receipt the platform refused.
type receiptAudit struct {
	EventType  string
	EventID    string
	ChargerID  int64
	OccurredAt time.Time
	TraceID    string
	Reason     string
}

// rejectReceipt makes a refusal durable without applying anything.
//
// Three things happen together: the idempotency claim is released, because
// nothing was applied and a refused receipt must not consume its id; the refusal
// is written to the order's audit trail, which is what "409 and audit, no
// buffering" means in practice; and the transaction commits with no state
// change. The returned error is the one the caller maps to a 4xx.
func (s *OrderStore) rejectReceipt(tx *sql.Tx, ctx context.Context, row orderRow, claimed bool, audit receiptAudit, want error) error {
	if claimed {
		if _, err := tx.ExecContext(ctx, `DELETE FROM idempotency_records
WHERE scope = $1 AND idempotency_key = $2`, receiptScope, strings.TrimSpace(audit.EventID)); err != nil {
			return err
		}
	}
	auditID, err := order.NewEventID()
	if err != nil {
		return err
	}
	payload, err := json.Marshal(map[string]any{
		"eventId":     audit.EventID,
		"eventType":   audit.EventType,
		"orderNo":     row.OrderNo,
		"chargerId":   strconvFormatInt64(audit.ChargerID),
		"occurredAt":  audit.OccurredAt.UTC().Format(time.RFC3339Nano),
		"orderStatus": row.Status,
		"reason":      audit.Reason,
		"traceId":     audit.TraceID,
	})
	if err != nil {
		return err
	}
	if _, err := tx.ExecContext(ctx, `INSERT INTO order_events (order_id, event_id, event_type, payload)
VALUES ($1, $2, 'CHARGER_EVENT_REJECTED', $3::jsonb)`, row.ID, auditID, string(payload)); err != nil {
		return err
	}
	if err := tx.Commit(); err != nil {
		return err
	}
	return want
}

// ConfirmStart moves STARTING → CHARGING on device confirmation.
//
// The receipt's fact time is what is written to started_at: the device reports
// when charging really began, so an offline or delayed report is billed in the
// window it happened in. The server clock only stamps updated_at and the
// idempotency record.
func (s *OrderStore) ConfirmStart(ctx context.Context, command order.ConfirmStartCommand) (order.Order, error) {
	if command.OccurredAt.IsZero() {
		return order.Order{}, fmt.Errorf("%w: occurredAt is required", order.ErrInvalidFactTime)
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return order.Order{}, err
	}
	defer func() { _ = tx.Rollback() }()

	claim, err := s.claimReceipt(tx, ctx, command.EventID, command.RequestHash)
	if err != nil {
		return order.Order{}, err
	}
	if claim.done {
		return decodeReplayedOrder(claim.body)
	}

	row, err := s.lockOrder(tx, ctx, command.OrderNo, 0)
	if err != nil {
		return order.Order{}, err
	}
	audit := receiptAudit{
		EventType: order.EventChargeStarted, EventID: strings.TrimSpace(command.EventID),
		ChargerID: command.ChargerID, OccurredAt: command.OccurredAt, TraceID: command.TraceID,
	}
	if command.ChargerID > 0 && command.ChargerID != row.ChargerID {
		audit.Reason = fmt.Sprintf("the order belongs to charger %d", row.ChargerID)
		return order.Order{}, s.rejectReceipt(tx, ctx, row, claim.claimed, audit, order.ErrChargerOrderMismatch)
	}
	result := row.Order
	if err := order.ValidateTransition(result.Status, order.StatusCharging); err != nil {
		audit.Reason = fmt.Sprintf("a start confirmation cannot be applied in status %s", result.Status)
		return order.Order{}, s.rejectReceipt(tx, ctx, row, claim.claimed, audit, err)
	}

	if _, err := tx.ExecContext(ctx, `UPDATE charging_orders
SET status = 'CHARGING', started_at = $2, version = version + 1, updated_at = CURRENT_TIMESTAMP
WHERE id = $1`, row.ID, command.OccurredAt.UTC()); err != nil {
		return order.Order{}, err
	}
	result.Status = order.StatusCharging
	result.UpdatedAt = s.clock().UTC()

	if err := s.appendOutbox(tx, ctx, order.EventChargeStarted, result.OrderNo, map[string]any{
		"orderNo":   result.OrderNo,
		"userId":    result.UserID,
		"stationId": result.StationID,
		"chargerId": result.ChargerID,
	}, command.TraceID); err != nil {
		return order.Order{}, err
	}

	if claim.claimed {
		body, err := json.Marshal(result)
		if err != nil {
			return order.Order{}, err
		}
		if err := s.finalizeIdempotency(tx, ctx, receiptScope, strings.TrimSpace(command.EventID), body); err != nil {
			return order.Order{}, err
		}
	}

	if err := tx.Commit(); err != nil {
		return order.Order{}, err
	}
	return result, nil
}

// ConfirmStop moves STOPPING → COMPLETED, computes the billed amount from the
// metered energy and the charger's current price, releases the charger and
// appends CHARGE_STOPPED plus ORDER_COMPLETED in the same transaction.
//
// The bill is settled across the time-of-use segments between started_at and
// the device's stop fact time against the tariff snapshot taken at start; the
// amount is frozen, a BILL_DETAIL row records the settlement breakdown and the
// payment stays PENDING (UC-U-09: the wallet is only touched when the user
// confirms the order). The charger is released here because the device has
// explicitly reported that it stopped - the one fact that makes releasing it
// safe.
func (s *OrderStore) ConfirmStop(ctx context.Context, command order.ConfirmStopCommand) (order.Order, error) {
	if command.OccurredAt.IsZero() {
		return order.Order{}, fmt.Errorf("%w: occurredAt is required", order.ErrInvalidFactTime)
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return order.Order{}, err
	}
	defer func() { _ = tx.Rollback() }()

	claim, err := s.claimReceipt(tx, ctx, command.EventID, command.RequestHash)
	if err != nil {
		return order.Order{}, err
	}
	if claim.done {
		return decodeReplayedOrder(claim.body)
	}

	row, err := s.lockOrder(tx, ctx, command.OrderNo, 0)
	if err != nil {
		return order.Order{}, err
	}
	audit := receiptAudit{
		EventType: order.EventChargeStopped, EventID: strings.TrimSpace(command.EventID),
		ChargerID: command.ChargerID, OccurredAt: command.OccurredAt, TraceID: command.TraceID,
	}
	if command.ChargerID > 0 && command.ChargerID != row.ChargerID {
		audit.Reason = fmt.Sprintf("the order belongs to charger %d", row.ChargerID)
		return order.Order{}, s.rejectReceipt(tx, ctx, row, claim.claimed, audit, order.ErrChargerOrderMismatch)
	}
	result := row.Order
	if err := order.ValidateTransition(result.Status, order.StatusCompleted); err != nil {
		// A stop confirmation that arrives before the start one lands here: the
		// order is still STARTING, so there is nothing to complete. It is
		// refused and audited rather than buffered, because a platform holding
		// device facts out of order cannot be sure which one is true.
		audit.Reason = fmt.Sprintf("a stop confirmation cannot be applied in status %s", result.Status)
		return order.Order{}, s.rejectReceipt(tx, ctx, row, claim.claimed, audit, order.ErrFactTimeOutOfOrder)
	}
	if !row.StartedAt.Valid {
		return order.Order{}, errors.New("postgres: completed order without a start time")
	}
	if command.OccurredAt.Before(row.StartedAt.Time) {
		audit.Reason = fmt.Sprintf("the stop fact time %s is earlier than the start %s",
			command.OccurredAt.UTC().Format(time.RFC3339), row.StartedAt.Time.UTC().Format(time.RFC3339))
		return order.Order{}, s.rejectReceipt(tx, ctx, row, claim.claimed, audit, order.ErrFactTimeOutOfOrder)
	}

	// The billed interval ends at the device's fact time, not at the moment the
	// platform happened to receive the receipt: the time-of-use split has to
	// follow the physical charge, otherwise a delayed report is billed in the
	// wrong window. The server clock only stamps updated_at.
	stoppedAt := command.OccurredAt.UTC()
	cfg := order.TariffConfig{
		PeakPrice:    row.PricePerKwhCents,
		OffPeak:      row.OffPeakPrice,
		WindowStart:  row.OffPeakStartHour,
		WindowEnd:    row.OffPeakEndHour,
		ServicePrice: row.ServicePricePerKwhCen,
	}
	// Both ends of the billed interval are UTC. The device reports a UTC fact time, and the tariff
	// window is defined in UTC hours; the row is read back through a database session whose zone is
	// the server's, so the times are converted explicitly. Billing a charge in the session's local
	// hours was a real defect: a charge at 03:00 UTC was priced as if it happened at 11:00 in a
	// +08 session, which is a different window entirely.
	bill := order.ComputeTOUBill(command.EnergyWh, row.StartedAt.Time.UTC(), stoppedAt, cfg)
	amount := bill.AmountCent

	if _, err := tx.ExecContext(ctx, `UPDATE charging_orders
SET status = 'COMPLETED', energy_wh = $2, amount_cents = $3, paid_cents = 0, payment_status = 'PENDING',
    stopped_at = $4, completed_at = $4, version = version + 1, updated_at = CURRENT_TIMESTAMP
WHERE id = $1`, row.ID, command.EnergyWh, amount, stoppedAt); err != nil {
		return order.Order{}, err
	}

	// Bill detail: the queryable settlement record (计费明细) for the frozen
	// bill — components, prices, segments and readings in one log entry.
	detail, err := json.Marshal(map[string]any{
		"orderNo":        result.OrderNo,
		"userId":         result.UserID,
		"energyWh":       command.EnergyWh,
		"meterStartWh":   command.MeterStartWh,
		"meterEndWh":     command.MeterEndWh,
		"servicePrice":   bill.ServicePrice,
		"electricityFee": bill.ElectricityFeeCen,
		"serviceFeeCent": bill.ServiceFeeCent,
		"amountCent":     bill.AmountCent,
		"segments":       bill.Segments,
	})
	if err != nil {
		return order.Order{}, err
	}
	detailID, err := order.NewEventID()
	if err != nil {
		return order.Order{}, err
	}
	if _, err := tx.ExecContext(ctx, `INSERT INTO order_events
    (order_id, event_id, event_type, payload)
VALUES ($1, $2, 'BILL_DETAIL', $3::jsonb)`,
		row.ID, detailID, string(detail)); err != nil {
		return order.Order{}, err
	}

	if _, err := tx.ExecContext(ctx, `UPDATE chargers SET status = 'IDLE', updated_at = CURRENT_TIMESTAMP WHERE id = $1`, result.ChargerID); err != nil {
		return order.Order{}, err
	}
	result.Status = order.StatusCompleted
	result.EnergyWh = command.EnergyWh
	result.AmountCent = amount
	result.PaymentStatus = "PENDING"
	result.PaidCent = 0
	result.UpdatedAt = s.clock().UTC()

	completedPayload := map[string]any{
		"orderNo":    result.OrderNo,
		"userId":     result.UserID,
		"stationId":  result.StationID,
		"chargerId":  result.ChargerID,
		"energyWh":   command.EnergyWh,
		"amountCent": amount,
	}
	if err := s.appendOutbox(tx, ctx, order.EventChargeStopped, result.OrderNo, map[string]any{
		"orderNo":   result.OrderNo,
		"userId":    result.UserID,
		"chargerId": result.ChargerID,
		"energyWh":  command.EnergyWh,
	}, command.TraceID); err != nil {
		return order.Order{}, err
	}
	if err := s.appendOutbox(tx, ctx, order.EventOrderCompleted, result.OrderNo, completedPayload, command.TraceID); err != nil {
		return order.Order{}, err
	}

	if claim.claimed {
		body, err := json.Marshal(result)
		if err != nil {
			return order.Order{}, err
		}
		if err := s.finalizeIdempotency(tx, ctx, receiptScope, strings.TrimSpace(command.EventID), body); err != nil {
			return order.Order{}, err
		}
	}

	if err := tx.Commit(); err != nil {
		return order.Order{}, err
	}
	return result, nil
}

// appendChargerCommand writes a device command event to the outbox so the
// B-line worker revokes a previously issued start command.
func (s *OrderStore) lockOrder(tx *sql.Tx, ctx context.Context, orderNo string, userID int64) (orderRow, error) {
	query := `SELECT ` + orderSelectColumns + ` FROM charging_orders WHERE order_no = $1`
	args := []any{orderNo}
	if userID > 0 {
		query += ` AND user_id = $2`
		args = append(args, userID)
	}
	query += ` FOR UPDATE`

	result, err := scanOrder(tx.QueryRowContext(ctx, query, args...))
	if errors.Is(err, sql.ErrNoRows) {
		return orderRow{}, order.ErrOrderNotFound
	}
	if err != nil {
		return orderRow{}, err
	}
	return result, nil
}

func (s *OrderStore) GetOrderByNo(ctx context.Context, userID int64, orderNo string) (order.Order, error) {
	const query = `SELECT ` + orderSelectColumns + ` FROM charging_orders WHERE order_no = $1 AND user_id = $2`
	row, err := scanOrder(s.db.QueryRowContext(ctx, query, orderNo, userID))
	if errors.Is(err, sql.ErrNoRows) {
		return order.Order{}, order.ErrOrderNotFound
	}
	if err != nil {
		return order.Order{}, err
	}
	return row.Order, nil
}

// orderByClause turns the contract's sort value into SQL. The value is checked
// against a fixed set here as well, so this function can never interpolate
// caller text into the statement even if a future caller forgets to validate.
func orderByClause(sort string) string {
	switch sort {
	case order.SortCreatedAtAsc:
		return "created_at ASC, id ASC"
	default:
		return "created_at DESC, id DESC"
	}
}

func (s *OrderStore) ListOrdersByUser(ctx context.Context, filter order.ListFilter) (order.OrderPage, error) {
	// Page rows and the total are separate queries sharing the filter, so
	// pages beyond the last row still report the real total. Filters cover
	// the minimum query surface: status, station, payment state and the
	// creation-time window.
	const filterSQL = `user_id = $1
  AND ($2 = '' OR status = $2)
  AND ($3::bigint IS NULL OR station_id = $3)
  AND ($4 = '' OR payment_status = $4)
  AND ($5::timestamptz IS NULL OR created_at >= $5)
  AND ($6::timestamptz IS NULL OR created_at < $6)`

	var stationID any
	if filter.StationIDSet {
		stationID = filter.StationID
	}
	var createdFrom, createdTo any
	if filter.CreatedFrom != nil {
		createdFrom = *filter.CreatedFrom
	}
	if filter.CreatedTo != nil {
		createdTo = *filter.CreatedTo
	}
	args := []any{filter.UserID, filter.Status, stationID, filter.PaymentStatus, createdFrom, createdTo}

	rows, err := s.db.QueryContext(ctx, `SELECT id, order_no, user_id, station_id, charger_id, status, amount_cents, paid_cents, payment_status, energy_wh, created_at, updated_at
FROM charging_orders
WHERE `+filterSQL+`
ORDER BY `+orderByClause(filter.Sort)+`
LIMIT $7 OFFSET $8`, append(args, filter.PageSize, (filter.Page-1)*filter.PageSize)...)
	if err != nil {
		return order.OrderPage{}, err
	}
	defer rows.Close()

	page := order.OrderPage{Meta: order.PageMeta{Page: filter.Page, PageSize: filter.PageSize}}
	for rows.Next() {
		var result order.Order
		var id int64
		if err := rows.Scan(&id, &result.OrderNo, &result.UserID, &result.StationID, &result.ChargerID,
			&result.Status, &result.AmountCent, &result.PaidCent, &result.PaymentStatus, &result.EnergyWh,
			&result.CreatedAt, &result.UpdatedAt); err != nil {
			return order.OrderPage{}, err
		}
		result.CreatedAt = result.CreatedAt.UTC()
		result.UpdatedAt = result.UpdatedAt.UTC()
		page.Items = append(page.Items, result)
	}
	if err := rows.Err(); err != nil {
		return order.OrderPage{}, err
	}

	if err := s.db.QueryRowContext(ctx, `SELECT count(*) FROM charging_orders WHERE `+filterSQL, args...).Scan(&page.Meta.Total); err != nil {
		return order.OrderPage{}, err
	}
	return page, nil
}

func decodeReplayedOrder(body []byte) (order.Order, error) {
	var result order.Order
	if err := json.Unmarshal(body, &result); err != nil {
		return order.Order{}, fmt.Errorf("decode idempotency replay: %w", err)
	}
	return result, nil
}
