package postgres

import (
	"context"
	"database/sql"
	"errors"

	"github.com/heguangV/charging-station-platform/backend/internal/admin"
)

// Station editing and the fleet-wide tariff, against PostgreSQL.

// UpdateStation replaces a station's editable profile under audit.
//
// It follows the same shape as the status change beside it: read the row under
// a lock, write, record the before and after in the same transaction. The lock
// is what makes the audit's "previous" values the ones this update actually
// replaced rather than whatever a concurrent change left behind.
func (s *AdminStore) UpdateStation(ctx context.Context, command admin.UpdateStationCommand) (admin.StationRecord, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return admin.StationRecord{}, err
	}
	defer func() { _ = tx.Rollback() }()

	var (
		previous            admin.StationRecord
		latitude, longitude sql.NullFloat64
	)
	err = tx.QueryRowContext(ctx, `SELECT id, code, name, address, status,
       COALESCE(latitude::float8, 0), COALESCE(longitude::float8, 0)
FROM stations WHERE id = $1 FOR UPDATE`, command.StationID).
		Scan(&previous.ID, &previous.Code, &previous.Name, &previous.Address, &previous.Status,
			&latitude, &longitude)
	if errors.Is(err, sql.ErrNoRows) {
		return admin.StationRecord{}, admin.ErrStationNotFound
	}
	if err != nil {
		return admin.StationRecord{}, err
	}
	previous.LatitudeE6 = toE6(latitude.Float64)
	previous.LongitudeE6 = toE6(longitude.Float64)

	// The columns hold degrees; the contract carries E6 integers.
	if _, err := tx.ExecContext(ctx, `UPDATE stations
SET name = $2, address = $3, latitude = $4, longitude = $5, updated_at = CURRENT_TIMESTAMP
WHERE id = $1`,
		command.StationID, command.Name, command.Address,
		float64(command.LatitudeE6)/1e6, float64(command.LongitudeE6)/1e6); err != nil {
		return admin.StationRecord{}, err
	}

	updated := previous
	updated.Name = command.Name
	updated.Address = command.Address
	updated.LatitudeE6 = command.LatitudeE6
	updated.LongitudeE6 = command.LongitudeE6

	// The code and the status are deliberately absent from both sides of the
	// audit: this endpoint does not touch them, and listing them would suggest
	// it might.
	if err := s.appendAudit(tx, ctx, command.AdminID, "station.update", "station",
		strconvFormatInt64(command.StationID), command.TraceID,
		map[string]any{
			"previous": stationProfile(previous),
			"new":      stationProfile(updated),
		}); err != nil {
		return admin.StationRecord{}, err
	}
	if err := tx.Commit(); err != nil {
		return admin.StationRecord{}, err
	}
	return updated, nil
}

// stationProfile is the audited part of a station.
func stationProfile(record admin.StationRecord) map[string]any {
	return map[string]any{
		"name":        record.Name,
		"address":     record.Address,
		"latitudeE6":  record.LatitudeE6,
		"longitudeE6": record.LongitudeE6,
	}
}

// GlobalTariff reads the distinct tariff configurations across the fleet.
//
// One query groups by the whole tariff rather than loading every charger: an
// operator needs to see how many chargers share a price, and a fleet of any
// size has far fewer distinct tariffs than devices.
func (s *AdminStore) GlobalTariff(ctx context.Context) ([]admin.GlobalTariff, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT price_per_kwh_cents,
       service_price_per_kwh_cents,
       off_peak_electricity_price_per_kwh_cents,
       off_peak_start_hour,
       off_peak_end_hour,
       count(*)
FROM chargers
GROUP BY 1, 2, 3, 4, 5
ORDER BY count(*) DESC, 1, 2, 3, 4, 5`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	configurations := make([]admin.GlobalTariff, 0, 8)
	for rows.Next() {
		var (
			configuration            admin.GlobalTariff
			offPeakPrice             sql.NullInt64
			offPeakStart, offPeakEnd sql.NullInt16
		)
		if err := rows.Scan(&configuration.ElectricityPriceCent, &configuration.ServicePriceCent,
			&offPeakPrice, &offPeakStart, &offPeakEnd, &configuration.ChargerCount); err != nil {
			return nil, err
		}
		if offPeakPrice.Valid {
			value := offPeakPrice.Int64
			configuration.OffPeakPriceCent = &value
		}
		if offPeakStart.Valid {
			value := offPeakStart.Int16
			configuration.OffPeakStartHour = &value
		}
		if offPeakEnd.Valid {
			value := offPeakEnd.Int16
			configuration.OffPeakEndHour = &value
		}
		configurations = append(configurations, configuration)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	return configurations, nil
}

// UpdateGlobalTariff writes one tariff to every charger, under audit.
//
// Every charger is included, a disabled one too: a device that is switched off
// today is switched on again tomorrow, and leaving it on last year's price would
// be discovered by a customer rather than by the operator.
//
// The whole table is written in one statement, which is the honest shape of the
// operation - it is a fleet-wide change, not a per-device one - and the row
// locks it takes are released with the transaction.
func (s *AdminStore) UpdateGlobalTariff(ctx context.Context, update admin.GlobalTariffUpdate) (admin.GlobalTariffResult, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return admin.GlobalTariffResult{}, err
	}
	defer func() { _ = tx.Rollback() }()

	// Counted before the write, so the audit records how many different prices
	// the change actually replaced.
	var previousConfigurations int64
	if err := tx.QueryRowContext(ctx, `SELECT count(*) FROM (
    SELECT 1 FROM chargers
    GROUP BY price_per_kwh_cents, service_price_per_kwh_cents,
             off_peak_electricity_price_per_kwh_cents, off_peak_start_hour, off_peak_end_hour
) AS distinct_tariffs`).Scan(&previousConfigurations); err != nil {
		return admin.GlobalTariffResult{}, err
	}

	result := tx.QueryRowContext(ctx, `WITH updated AS (
    UPDATE chargers
    SET price_per_kwh_cents = $1,
        service_price_per_kwh_cents = $2,
        off_peak_electricity_price_per_kwh_cents = $3,
        off_peak_start_hour = $4,
        off_peak_end_hour = $5,
        updated_at = CURRENT_TIMESTAMP
    RETURNING 1
)
SELECT count(*) FROM updated`,
		update.ElectricityPriceCent, update.ServicePriceCent,
		update.OffPeakPriceCent, update.OffPeakStartHour, update.OffPeakEndHour)

	applied := admin.GlobalTariffResult{
		PreviousConfigurations: previousConfigurations,
		ElectricityPriceCent:   update.ElectricityPriceCent,
		ServicePriceCent:       update.ServicePriceCent,
		OffPeakPriceCent:       update.OffPeakPriceCent,
		OffPeakStartHour:       update.OffPeakStartHour,
		OffPeakEndHour:         update.OffPeakEndHour,
	}
	if err := result.Scan(&applied.AffectedChargers); err != nil {
		return admin.GlobalTariffResult{}, err
	}

	if err := s.appendAudit(tx, ctx, update.AdminID, "tariff.global-update", "tariff",
		"GLOBAL", update.TraceID,
		map[string]any{
			"reason":                 update.Reason,
			"affectedChargers":       applied.AffectedChargers,
			"previousConfigurations": previousConfigurations,
			"new": map[string]any{
				"electricityPrice": applied.ElectricityPriceCent,
				"servicePrice":     applied.ServicePriceCent,
				"offPeakPrice":     applied.OffPeakPriceCent,
				"offPeakStartHour": applied.OffPeakStartHour,
				"offPeakEndHour":   applied.OffPeakEndHour,
			},
		}); err != nil {
		return admin.GlobalTariffResult{}, err
	}
	if err := tx.Commit(); err != nil {
		return admin.GlobalTariffResult{}, err
	}
	return applied, nil
}
