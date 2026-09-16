package station

import (
	"context"
	"errors"
)

// The station view the assistant recommends from.
//
// It is a separate, narrower read rather than a wider use of Station: a
// recommendation has to state the price split and the charger mix, and the
// list payload deliberately carries only the cheapest total. Adding those
// columns to the list would make every caller pay for them.
//
// The two are kept consistent by construction: the split is taken from the
// same cheapest charger that decides MinPriceCentPerKwh, so the parts always
// add up to the whole and a card may show either.

// Summary is one station as a recommendation.
type Summary struct {
	ID          int64
	Code        string
	Name        string
	Address     string
	Status      string
	LatitudeE6  int64
	LongitudeE6 int64

	// ChargerCount is every charger; IdleChargerCount those free right now;
	// OperationalChargerCount those not disabled, which is what a driver can
	// actually use today.
	ChargerCount            int64
	IdleChargerCount        int64
	OperationalChargerCount int64
	FastChargerCount        int64
	SlowChargerCount        int64
	// ChargerTypes lists the connector types present, in a stable order.
	ChargerTypes []string

	// TotalPriceCentPerKwh is electricity plus service, and is the sum of the
	// two fields below.
	TotalPriceCentPerKwh       int64
	ElectricityPriceCentPerKwh int64
	ServicePriceCentPerKwh     int64

	// DistanceMeter is meaningful only when HasDistance is set: a keyword-only
	// search has no distance, and reporting zero would read as "right here".
	DistanceMeter int64
	HasDistance   bool
}

// ChargerInfo is one charger inside a station detail.
type ChargerInfo struct {
	Code      string
	Type      string
	PowerWatt int64
	Status    string
}

// SummaryDetail is a station with a page of its chargers.
type SummaryDetail struct {
	Summary  Summary
	Chargers []ChargerInfo
	// ChargerTotal is how many chargers match, which may exceed the page size.
	ChargerTotal int64
}

// SummaryFilter is a validated recommendation query. Latitude/Longitude are
// only applied when HasLocation is set.
type SummaryFilter struct {
	Keyword            string
	Latitude           float64
	Longitude          float64
	HasLocation        bool
	RadiusMeters       int64 // 0 = unlimited
	ConnectorType      string
	MaxPriceCentPerKwh int64 // 0 = no cap
	MinIdleChargers    int64
	Limit              int
}

// MaxSummaryLimit bounds a recommendation page. The assistant never needs more
// than a handful of stations, and an unbounded limit would let one question
// pull the whole table.
const MaxSummaryLimit = 10

var (
	// ErrInvalidSummaryFilter reports a filter outside its contract bounds.
	ErrInvalidSummaryFilter = errors.New("station: invalid summary filter")
	// ErrInvalidSummaryLimit reports a limit outside 1..MaxSummaryLimit.
	ErrInvalidSummaryLimit = errors.New("station: invalid summary limit")
)

// SummaryReader is the persistence boundary for recommendation reads.
//
// It is separate from Reader so that adding the assistant did not widen the
// interface every existing caller already implements.
type SummaryReader interface {
	SearchStationSummaries(ctx context.Context, filter SummaryFilter) ([]Summary, error)
	GetStationSummary(ctx context.Context, stationID int64, chargerType string, chargerLimit int) (SummaryDetail, error)
}

// SummaryService validates recommendation queries and delegates to a reader.
//
// It is separate from Service because the two read different things for
// different callers: Service serves the contract station list, and this serves
// the assistant's recommendations. Sharing one service would have meant one
// validation surface for two payloads.
type SummaryService struct {
	reader SummaryReader
}

// NewSummaryService wires the service to its reader.
func NewSummaryService(reader SummaryReader) (*SummaryService, error) {
	if reader == nil {
		return nil, errors.New("station: a summary reader is required")
	}
	return &SummaryService{reader: reader}, nil
}

// SearchSummaries returns the stations matching the filter, nearest first when
// a position is present.
func (s *SummaryService) SearchSummaries(ctx context.Context, filter SummaryFilter) ([]Summary, error) {
	if filter.Limit <= 0 || filter.Limit > MaxSummaryLimit {
		return nil, ErrInvalidSummaryLimit
	}
	if filter.HasLocation {
		if filter.Latitude < -90 || filter.Latitude > 90 ||
			filter.Longitude < -180 || filter.Longitude > 180 {
			return nil, ErrInvalidSummaryFilter
		}
	}
	if filter.RadiusMeters < 0 || filter.MaxPriceCentPerKwh < 0 || filter.MinIdleChargers < 0 {
		return nil, ErrInvalidSummaryFilter
	}
	if filter.ConnectorType != "" && filter.ConnectorType != ConnectorAC && filter.ConnectorType != ConnectorDC {
		return nil, ErrInvalidSummaryFilter
	}
	if len(filter.Keyword) > 100 {
		return nil, ErrInvalidSummaryFilter
	}
	return s.reader.SearchStationSummaries(ctx, filter)
}

// GetStationSummary returns one station with a page of its chargers.
func (s *SummaryService) GetStationSummary(ctx context.Context, stationID int64, chargerType string, chargerLimit int) (SummaryDetail, error) {
	if stationID < 1 {
		return SummaryDetail{}, ErrInvalidStationID
	}
	if chargerLimit <= 0 || chargerLimit > MaxSummaryLimit {
		return SummaryDetail{}, ErrInvalidSummaryLimit
	}
	if chargerType != "" && chargerType != ConnectorAC && chargerType != ConnectorDC {
		return SummaryDetail{}, ErrInvalidSummaryFilter
	}
	return s.reader.GetStationSummary(ctx, stationID, chargerType, chargerLimit)
}
