package postgres

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"strconv"

	"github.com/heguangV/charging-station-platform/backend/internal/admin"
)

// Batch charger creation, against PostgreSQL.
//
// The whole batch is one INSERT ... SELECT over three arrays. That is what makes
// the operation atomic in the way the feature needs: a code that collides with
// an existing device, or a station that was disabled between the check and the
// write, aborts the single statement, and the transaction rolls back with
// nothing created. Looping over per-device inserts inside one transaction would
// end in the same state, but it would also take N round trips and leave the
// partial state visible to a concurrent reader for the duration.

// CreateChargers creates a batch of devices at one station, under audit and one
// idempotency key.
func (s *AdminStore) CreateChargers(ctx context.Context, command admin.CreateChargersCommand) (admin.ChargerBatchResult, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return admin.ChargerBatchResult{}, err
	}
	defer func() { _ = tx.Rollback() }()

	scope := fmt.Sprintf("admin:%d:charger:batch", command.AdminID)
	replay, replayed, err := s.claimIdempotency(tx, ctx, scope, command.IdempotencyKey, command.RequestHash)
	if err != nil {
		return admin.ChargerBatchResult{}, err
	}
	if replayed {
		// The batch already ran. The recorded answer is returned unchanged, so a
		// retried request neither creates a second set of devices nor reports a
		// different number than the first attempt did.
		var result admin.ChargerBatchResult
		if err := json.Unmarshal(replay, &result); err != nil {
			return admin.ChargerBatchResult{}, fmt.Errorf("decode idempotency replay: %w", err)
		}
		return result, nil
	}

	// The station has to exist. Its status is deliberately not checked: a device
	// bank is commissioned before the station opens, and refusing to record
	// hardware because the station is not yet switched on would push the work
	// back to whoever is standing in front of the cabinet.
	var stationCode string
	err = tx.QueryRowContext(ctx, `SELECT code FROM stations WHERE id = $1`, command.StationID).Scan(&stationCode)
	if errors.Is(err, sql.ErrNoRows) {
		return admin.ChargerBatchResult{}, admin.ErrStationNotFound
	}
	if err != nil {
		return admin.ChargerBatchResult{}, err
	}

	codes := make([]string, 0, len(command.Chargers))
	connectors := make([]string, 0, len(command.Chargers))
	powers := make([]int64, 0, len(command.Chargers))
	for _, draft := range command.Chargers {
		codes = append(codes, draft.Code)
		connectors = append(connectors, draft.ConnectorType)
		powers = append(powers, draft.PowerWatt)
	}

	rows, err := tx.QueryContext(ctx, `INSERT INTO chargers (station_id, code, connector_type, power_watt, status)
SELECT $1, code, connector_type, power_watt, 'IDLE'
FROM unnest($2::text[], $3::text[], $4::bigint[]) AS drafted(code, connector_type, power_watt)
RETURNING id, code, connector_type, power_watt, status`,
		command.StationID, codes, connectors, powers)
	if err != nil {
		// The unique index on (station_id, code) is the authority for a code that
		// already exists. It is a business answer (409), not an internal error, and
		// the failed statement has already taken the whole batch with it.
		if isUniqueViolation(err) {
			return admin.ChargerBatchResult{}, fmt.Errorf("%w: a code in this batch already exists at the station",
				admin.ErrDuplicateChargerCode)
		}
		return admin.ChargerBatchResult{}, err
	}
	defer rows.Close()

	created := make([]admin.ChargerRecord, 0, len(command.Chargers))
	for rows.Next() {
		var record admin.ChargerRecord
		if err := rows.Scan(&record.ID, &record.Code, &record.Type, &record.PowerWatt, &record.Status); err != nil {
			return admin.ChargerBatchResult{}, err
		}
		record.StationID = command.StationID
		created = append(created, record)
	}
	if err := rows.Err(); err != nil {
		return admin.ChargerBatchResult{}, err
	}
	// A RETURNING that produced fewer rows than were sent means the insert did
	// not apply everything; committing would hide that, so the batch is refused.
	if len(created) != len(command.Chargers) {
		return admin.ChargerBatchResult{}, fmt.Errorf("postgres: created %d of %d chargers", len(created), len(command.Chargers))
	}

	result := admin.ChargerBatchResult{
		StationID:    command.StationID,
		ChargerCount: int64(len(created)),
		Created:      created,
	}

	// The audit records the count and the codes. A hundred strings is a bounded
	// payload, and "which devices did that batch create" is the question an
	// operator asks when the count disagrees with the cabinet.
	auditCodes := make([]string, 0, len(created))
	for _, record := range created {
		auditCodes = append(auditCodes, record.Code)
	}
	if err := s.appendAudit(tx, ctx, command.AdminID, "charger.batch-create", "station",
		strconv.FormatInt(command.StationID, 10), command.TraceID,
		map[string]any{
			"stationCode":  stationCode,
			"chargerCount": result.ChargerCount,
			"codes":        auditCodes,
		}); err != nil {
		return admin.ChargerBatchResult{}, err
	}

	body, err := json.Marshal(result)
	if err != nil {
		return admin.ChargerBatchResult{}, err
	}
	if err := s.finalizeIdempotency(tx, ctx, scope, command.IdempotencyKey, body); err != nil {
		return admin.ChargerBatchResult{}, err
	}
	if err := tx.Commit(); err != nil {
		return admin.ChargerBatchResult{}, err
	}
	return result, nil
}
