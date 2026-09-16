package admin

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"unicode"
	"unicode/utf8"
)

// Batch charger creation.
//
// Commissioning a station means entering a whole bank of devices at once: a
// station with twenty bays is not twenty trips through a form. This is that
// operation, and its whole point is that it is all-or-nothing - a batch that
// half-succeeded would leave an operator counting rows to work out which
// devices exist, and a station whose device list disagrees with the physical
// cabinet is worse than one that was never created.
//
// The shape follows the old console's batch endpoint: one station, one to a
// hundred drafts, and a code per draft. What it does not carry is a price. A
// device is created at the schema's default and takes its tariff from the
// tariff endpoints, because guessing a price for hardware nobody has priced
// would put a number on a bill that nobody chose.

const (
	// MaxBatchChargers bounds one batch. It is the old endpoint's bound and it is
	// also the point where an operator is better served by a script than by a
	// dialog.
	MaxBatchChargers = 100
	// maxChargerCodeLength matches the column's contract and the old endpoint.
	maxChargerCodeLength = 32
	// maxChargerPowerWatt is the schema's upper bound.
	maxChargerPowerWatt = 1_000_000
)

// Charger batch errors.
var (
	// ErrInvalidChargerBatch reports a batch the platform will not create: an
	// empty one, an over-long one, or a draft outside its field bounds.
	ErrInvalidChargerBatch = errors.New("admin: invalid charger batch")
	// ErrDuplicateChargerCode reports a code used twice, either twice in the
	// request or once already at the station. It maps to 409 ALREADY_EXISTS: the
	// request is well formed, it just collides with what exists.
	ErrDuplicateChargerCode = errors.New("admin: duplicate charger code")
)

// ChargerDraft is one device to create.
//
// It carries only what an operator types in: which bay it is, what kind of
// connector it has, and how much power it can deliver. Status, price and version
// are the platform's to decide.
type ChargerDraft struct {
	Code string
	// ConnectorType is AC or DC, the platform's own words, not the console's
	// 0/1 shorthand.
	ConnectorType string
	PowerWatt     int64
}

// CreateChargersCommand carries a validated batch request.
type CreateChargersCommand struct {
	AdminID        int64
	StationID      int64
	Chargers       []ChargerDraft
	IdempotencyKey string
	RequestHash    string
	TraceID        string
}

// ChargerRecord is one created device as the console receives it.
type ChargerRecord struct {
	ID        int64  `json:"id"`
	StationID int64  `json:"stationId"`
	Code      string `json:"code"`
	Type      string `json:"type"`
	PowerWatt int64  `json:"powerWatt"`
	Status    string `json:"status"`
}

// ChargerBatchResult is what a batch reports back.
type ChargerBatchResult struct {
	StationID int64 `json:"stationId"`
	// ChargerCount is how many devices this request created. It equals
	// len(Created) and is stated separately so a client that only needs the
	// count does not have to walk the list.
	ChargerCount int64           `json:"chargerCount"`
	Created      []ChargerRecord `json:"created"`
}

// CreateChargers creates a batch of devices at one station.
//
// Everything is checked before the store is called, so a request that cannot
// succeed never opens a transaction. What the store then guarantees is that the
// insert is one statement: a code that collides with an existing device aborts
// the whole batch rather than creating the devices before it.
func (s *Service) CreateChargers(ctx context.Context, command CreateChargersCommand) (ChargerBatchResult, error) {
	if command.AdminID < 1 {
		return ChargerBatchResult{}, ErrInvalidAdminActor
	}
	if command.StationID < 1 {
		return ChargerBatchResult{}, fmt.Errorf("%w: station id", ErrInvalidChargerBatch)
	}
	if len(command.Chargers) < 1 || len(command.Chargers) > MaxBatchChargers {
		return ChargerBatchResult{}, fmt.Errorf("%w: a batch carries 1 to %d devices", ErrInvalidChargerBatch, MaxBatchChargers)
	}

	seen := make(map[string]bool, len(command.Chargers))
	normalized := make([]ChargerDraft, 0, len(command.Chargers))
	for index := range command.Chargers {
		draft := command.Chargers[index]
		draft.Code = strings.TrimSpace(draft.Code)
		if !validChargerCode(draft.Code) {
			return ChargerBatchResult{}, fmt.Errorf("%w: device %d has an unusable code", ErrInvalidChargerBatch, index+1)
		}
		// A code repeated inside one request is a duplicate like any other. It is
		// caught here rather than by the unique index so the answer names the
		// code instead of leaving the operator to compare two rows by eye.
		if seen[draft.Code] {
			return ChargerBatchResult{}, fmt.Errorf("%w: %s appears twice in the request", ErrDuplicateChargerCode, draft.Code)
		}
		seen[draft.Code] = true

		if draft.ConnectorType != "AC" && draft.ConnectorType != "DC" {
			return ChargerBatchResult{}, fmt.Errorf("%w: device %s has connector type %q", ErrInvalidChargerBatch, draft.Code, draft.ConnectorType)
		}
		if draft.PowerWatt < 1 || draft.PowerWatt > maxChargerPowerWatt {
			return ChargerBatchResult{}, fmt.Errorf("%w: device %s has power outside 1..%d", ErrInvalidChargerBatch, draft.Code, maxChargerPowerWatt)
		}
		normalized = append(normalized, draft)
	}
	command.Chargers = normalized
	return s.store.CreateChargers(ctx, command)
}

// validChargerCode reports whether a code can be stored and read back.
//
// The bound is the column's. Control characters are refused as well: a code
// carrying a newline or a tab would be invisible in a device list and would make
// a CSV export lie about how many devices there are.
func validChargerCode(code string) bool {
	if code == "" || utf8.RuneCountInString(code) > maxChargerCodeLength {
		return false
	}
	for _, character := range code {
		if unicode.IsControl(character) {
			return false
		}
	}
	return true
}
