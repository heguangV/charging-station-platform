// Package order implements the P0 order core domain: creation, the charging
// state machine, start/stop requests, idempotency and amount calculation.
// State transitions are validated here and re-validated inside the store
// transaction; the database constraints (uq_orders_charger_active,
// uq_orders_user_active) are the final concurrency guards.
package order

import (
	"crypto/rand"
	"encoding/hex"
	"errors"
	"fmt"
	"sort"
	"strings"
	"time"
)

// Status values from the frozen contract OrderStatus enum.
const (
	StatusCreated   = "CREATED"
	StatusStarting  = "STARTING"
	StatusCharging  = "CHARGING"
	StatusStopping  = "STOPPING"
	StatusCompleted = "COMPLETED"
	StatusCancelled = "CANCELLED"
	StatusExpired   = "EXPIRED"
	StatusFailed    = "FAILED"
)

// ActiveStatuses occupy a charger and block a user from opening a new flow.
var ActiveStatuses = []string{StatusCreated, StatusStarting, StatusCharging, StatusStopping}

// Event types from the frozen event contract (backend-parallel-development
// §4.2). A drift test pins these against the B-line event package.
const (
	EventOrderCreated            = "ORDER_CREATED"
	EventChargeStartRequested    = "CHARGE_START_REQUESTED"
	EventChargeStarted           = "CHARGE_STARTED"
	EventChargeStopRequested     = "CHARGE_STOP_REQUESTED"
	EventChargeStopped           = "CHARGE_STOPPED"
	EventOrderCompleted          = "ORDER_COMPLETED"
	EventChargerCommandRequested = "CHARGER_COMMAND_REQUESTED"
	// EventChargerCommandCompleted is produced when the device answers a command. It is written
	// with the state change it describes, in the same transaction as the charger row update.
	EventChargerCommandCompleted = "CHARGER_COMMAND_COMPLETED"

	// Device command actions. RESTART is the station-level maintenance action; the two charge
	// actions are produced by the order transactions and must not be impersonated by RESTART.
	CommandRestart       = "RESTART"
	CommandStartCharging = "START_CHARGING"
	CommandStopCharging  = "STOP_CHARGING"

	// Device command outcomes (BE-I-02). This enum is the only thing a device verdict may be:
	// an accepted command that completed, or an explicit refusal. A timeout and an unknown status
	// are NOT outcomes - they are retried, because turning them into a failure would fail an order
	// or park a charger on a guess instead of on something the device said.
	CommandResultCompleted = "COMPLETED"
	CommandResultFailed    = "FAILED"
)

// IsCommandResult reports whether a value is one of the two frozen device outcomes.
func IsCommandResult(result string) bool {
	switch strings.ToUpper(strings.TrimSpace(result)) {
	case CommandResultCompleted, CommandResultFailed:
		return true
	}
	return false
}

// MinStartBalanceCents is the city-wide minimum start balance (BR-04: 5.00元).
const MinStartBalanceCents = 500

// DefaultReservationDuration is the UC-U-07 device hold after a user confirms
// a quote. Deployments may override the janitor duration, and the API store is
// configured with the same value so the exposed deadline and enforcement
// cannot drift apart.
const DefaultReservationDuration = 15 * time.Minute

var validStatuses = map[string]bool{
	StatusCreated: true, StatusStarting: true, StatusCharging: true, StatusStopping: true,
	StatusCompleted: true, StatusCancelled: true, StatusExpired: true, StatusFailed: true,
}

// transitions maps a status to the statuses reachable from it. Terminal
// statuses (COMPLETED, CANCELLED, EXPIRED, FAILED) have no outgoing edges.
var transitions = map[string]map[string]bool{
	StatusCreated: {
		StatusStarting:  true,
		StatusCancelled: true,
		StatusExpired:   true,
		StatusFailed:    true,
	},
	StatusStarting: {
		StatusCharging:  true,
		StatusCancelled: true,
		StatusFailed:    true,
	},
	StatusCharging: {
		StatusStopping: true,
		StatusFailed:   true,
	},
	StatusStopping: {
		StatusCompleted: true,
		StatusFailed:    true,
	},
}

// ErrInvalidStateTransition reports an illegal status jump (409, code 15).
var ErrInvalidStateTransition = errors.New("order: invalid state transition")

// CanTransition reports whether from → to is part of the P0 state machine.
func CanTransition(from, to string) bool {
	return transitions[from][to]
}

// ValidateTransition returns ErrInvalidStateTransition for illegal jumps.
func ValidateTransition(from, to string) error {
	if !CanTransition(from, to) {
		return fmt.Errorf("%w: %s -> %s", ErrInvalidStateTransition, from, to)
	}
	return nil
}

// IsValidStatus reports whether the value is part of the contract enum.
func IsValidStatus(status string) bool {
	return validStatuses[status]
}

// IsActive reports whether the status keeps the flow open.
func IsActive(status string) bool {
	for _, active := range ActiveStatuses {
		if status == active {
			return true
		}
	}
	return false
}

// Bill is the BR-05 fee breakdown: 累计电量 ×（电费快照 + 服务费快照）.
// ElectricityFee is the sum of the per-segment fees and Amount is exactly
// ElectricityFee + ServiceFee, so the components always add up to the total.
type Bill struct {
	EnergyWh          int64        `json:"energyWh"`
	ElectricityPrice  int64        `json:"electricityPrice"`
	ServicePrice      int64        `json:"servicePrice"`
	ElectricityFeeCen int64        `json:"electricityFeeCent"`
	ServiceFeeCent    int64        `json:"serviceFeeCent"`
	AmountCent        int64        `json:"amountCent"`
	Segments          []SegmentFee `json:"segments,omitempty"`
}

// SegmentFee is one time-of-use slice of the charge: the energy accrued
// inside the slice, its tariff and the resulting fee.
type SegmentFee struct {
	From     time.Time `json:"from"`
	To       time.Time `json:"to"`
	EnergyWh int64     `json:"energyWh"`
	Price    int64     `json:"price"`
	FeeCent  int64     `json:"feeCent"`
}

// AmountCents bills energyWh at pricePerKwhCents (cents per kWh), rounding
// half up with pure integer math: 1500Wh at 100 cents/kWh = 150 cents.
func AmountCents(energyWh, pricePerKwhCents int64) int64 {
	if energyWh < 0 {
		energyWh = 0
	}
	if pricePerKwhCents < 0 {
		pricePerKwhCents = 0
	}
	return (energyWh*pricePerKwhCents + 500) / 1000
}

// ComputeBill applies BR-05 for a flat tariff: each component fee rounds
// half up, and the amount is the exact sum of the components.
func ComputeBill(energyWh, electricityPrice, servicePrice int64) Bill {
	electricityFee := AmountCents(energyWh, electricityPrice)
	serviceFee := AmountCents(energyWh, servicePrice)
	return Bill{
		EnergyWh:          energyWh,
		ElectricityPrice:  electricityPrice,
		ServicePrice:      servicePrice,
		ElectricityFeeCen: electricityFee,
		ServiceFeeCent:    serviceFee,
		AmountCent:        electricityFee + serviceFee,
	}
}

// TariffConfig is the tariff snapshot taken when charging starts.
type TariffConfig struct {
	PeakPrice    int64
	OffPeak      *int64 // nil: no time-of-use tariff
	WindowStart  *int16 // hour of day, may span midnight
	WindowEnd    *int16
	ServicePrice int64
}

// defaultBillingLocation is the wall-clock timezone the off-peak window is expressed in when the
// deployment does not configure one.
//
// A fixed +08, not time.Local and not UTC: the product bills Chinese operators, whose off-peak
// window is local night (the seed configures 23:00-07:00), and China has had no DST since 1991.
// Resolving those hours against UTC (the previous behaviour) moved a 23:00-07:00 window to
// 07:00-15:00 local time, so an afternoon charge was billed at the off-peak price.
var defaultBillingLocation = time.FixedZone("CST", 8*60*60)

// DefaultBillingLocation returns the fallback billing timezone. Callers that hold configuration
// (NCS_BILLING_TZ) should pass their own location instead.
func DefaultBillingLocation() *time.Location { return defaultBillingLocation }

// UnitPriceCents is the electricity price that applies at one instant plus the service fee: the
// per-kWh figure a client needs to estimate a charge that is still running.
func UnitPriceCents(at time.Time, cfg TariffConfig) int64 {
	return EffectiveElectricityPrice(at, cfg) + cfg.ServicePrice
}

// EffectiveElectricityPrice resolves the tariff for one instant: the
// off-peak price applies inside the configured hour window (which may span
// midnight), otherwise the peak price. The window is read in the location of
// the instant it is given, so the caller decides the billing timezone.
func EffectiveElectricityPrice(start time.Time, cfg TariffConfig) int64 {
	if cfg.OffPeak == nil || cfg.WindowStart == nil || cfg.WindowEnd == nil || *cfg.WindowStart == *cfg.WindowEnd {
		return cfg.PeakPrice
	}
	hour := int16(start.Hour())
	var inWindow bool
	if *cfg.WindowStart < *cfg.WindowEnd {
		inWindow = hour >= *cfg.WindowStart && hour < *cfg.WindowEnd
	} else {
		inWindow = hour >= *cfg.WindowStart || hour < *cfg.WindowEnd
	}
	if inWindow {
		return int64(*cfg.OffPeak)
	}
	return cfg.PeakPrice
}

// SplitChargeSegments cuts the charging interval at the tariff boundaries
// (the off-peak window edges and midnight) so every slice settles at one
// price. With no time-of-use tariff the whole interval is one slice.
func SplitChargeSegments(start, end time.Time, cfg TariffConfig) [][2]time.Time {
	if !end.After(start) {
		end = start.Add(time.Second)
	}

	boundaries := make([]time.Time, 0, 8)
	if cfg.OffPeak != nil && cfg.WindowStart != nil && cfg.WindowEnd != nil && *cfg.WindowStart != *cfg.WindowEnd {
		dayStart := time.Date(start.Year(), start.Month(), start.Day(), 0, 0, 0, 0, start.Location())
		for day := -1; day <= int(end.Sub(start).Hours()/24)+1; day++ {
			base := dayStart.AddDate(0, 0, day)
			boundaries = append(boundaries,
				base.Add(time.Duration(*cfg.WindowStart)*time.Hour),
				base.Add(time.Duration(*cfg.WindowEnd)*time.Hour),
			)
		}
		// The day walk emits both window edges per day without ordering; a
		// midnight-spanning window would interleave them and mis-split the
		// bill. Monotonic boundaries are the invariant the slicing loop and
		// the proportional energy split depend on.
		sort.Slice(boundaries, func(i, j int) bool { return boundaries[i].Before(boundaries[j]) })
	}

	slices := make([][2]time.Time, 0, len(boundaries)+1)
	cursor := start
	for _, boundary := range boundaries {
		if boundary.After(cursor) && boundary.Before(end) {
			slices = append(slices, [2]time.Time{cursor, boundary})
			cursor = boundary
		}
	}
	slices = append(slices, [2]time.Time{cursor, end})
	return slices
}

// ComputeTOUBill settles a charge across time-of-use segments. The device
// reports the total energy, so the slices share it in proportion to their
// share of the charging duration; the largest-remainder method makes the
// segment energies sum exactly to the metered total. Each slice rounds its
// own fee half up, and Amount is exactly ElectricityFee + ServiceFee.
func ComputeTOUBill(energyWh int64, start, end time.Time, cfg TariffConfig) Bill {
	slices := SplitChargeSegments(start, end, cfg)
	durations := make([]time.Duration, len(slices))
	var total time.Duration
	for index, slice := range slices {
		durations[index] = slice[1].Sub(slice[0])
		if durations[index] <= 0 {
			durations[index] = time.Millisecond
		}
		total += durations[index]
	}

	// Largest-remainder distribution of the metered energy.
	shares := make([]int64, len(slices))
	reminders := make([]int64, len(slices))
	distributed := int64(0)
	for index, duration := range durations {
		shares[index] = energyWh * int64(duration) / int64(total)
		reminders[index] = energyWh * int64(duration) % int64(total)
		distributed += shares[index]
	}
	leftover := energyWh - distributed
	for leftover > 0 {
		best := -1
		var bestRemainder int64 = -1
		for index, reminder := range reminders {
			if reminder > bestRemainder {
				bestRemainder = reminder
				best = index
			}
		}
		if best < 0 {
			break
		}
		shares[best]++
		reminders[best] = -1
		leftover--
	}

	segments := make([]SegmentFee, 0, len(slices))
	electricityFee := int64(0)
	for index, slice := range slices {
		fee := AmountCents(shares[index], EffectiveElectricityPrice(slice[0], cfg))
		segments = append(segments, SegmentFee{
			From:     slice[0],
			To:       slice[1],
			EnergyWh: shares[index],
			Price:    EffectiveElectricityPrice(slice[0], cfg),
			FeeCent:  fee,
		})
		electricityFee += fee
	}

	serviceFee := AmountCents(energyWh, cfg.ServicePrice)
	return Bill{
		EnergyWh:          energyWh,
		ServicePrice:      cfg.ServicePrice,
		ElectricityFeeCen: electricityFee,
		ServiceFeeCent:    serviceFee,
		AmountCent:        electricityFee + serviceFee,
		Segments:          segments,
	}
}

// NewOrderNo mints a unique-enough business order number: ORD + UTC timestamp
// + 8 random hex chars (25 chars, inside the 8..64 contract bounds). The
// database unique constraint is the real guard.
func NewOrderNo(now time.Time) (string, error) {
	random, err := randomHex(4)
	if err != nil {
		return "", err
	}
	return "ORD" + now.UTC().Format("20060102150405") + random, nil
}

// NewEventID mints an event identifier in the contract "evt_" style.
func NewEventID() (string, error) {
	random, err := randomHex(16)
	if err != nil {
		return "", err
	}
	return "evt_" + random, nil
}

func randomHex(bytes int) (string, error) {
	buffer := make([]byte, bytes)
	if _, err := rand.Read(buffer); err != nil {
		return "", fmt.Errorf("order: generate random: %w", err)
	}
	return hex.EncodeToString(buffer), nil
}
