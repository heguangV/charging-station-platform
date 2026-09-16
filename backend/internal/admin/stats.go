package admin

import (
	"context"
	"errors"
	"fmt"
	"math"
)

// The operations statistics the console reads.
//
// These are the Go replacement for the analytics the old service precomputed
// into a dashboard snapshot. The snapshot is gone and the numbers are computed
// on demand, because a precomputed snapshot has to be rebuilt on a timer, goes
// stale between rebuilds, and answers with data that may be hours old - which
// is a poor trade for an aggregate PostgreSQL answers in milliseconds.
//
// Two definitions are carried over from the old service unchanged, because they
// are business rules rather than implementation details:
//
//   - Revenue is the bill of a COMPLETED order, attributed to the moment the
//     charge stopped. That is when the amount was frozen, and it is the only
//     timestamp that cannot change afterwards.
//   - A charger is operational when it is idle or occupied. A faulty or
//     disabled device cannot serve, and a restarting one is not available at
//     the moment it is counted.

// Bucket names for the revenue series.
const (
	StatsBucketHour = "hour"
	StatsBucketDay  = "day"
)

const (
	secondsPerHour = int64(3600)
	secondsPerDay  = int64(86400)

	// MaxStatsRangeSeconds bounds one statistics query at 90 days, matching the
	// window the console offers and the ML feature window. An unbounded range on
	// an hour-bucketed series would return years of points to a chart that can
	// draw a few hundred.
	MaxStatsRangeSeconds = 90 * secondsPerDay
)

// Statistics errors. Handlers map all of them to 400: they describe a query the
// caller wrote, not a failure of the platform.
var (
	// ErrInvalidStatsRange reports a missing, inverted or over-long time range.
	ErrInvalidStatsRange = errors.New("admin: invalid statistics time range")
	// ErrInvalidStatsBucket reports a bucket outside the contract enum.
	ErrInvalidStatsBucket = errors.New("admin: invalid statistics bucket")
	// ErrInvalidStatsStation reports a station filter below 1.
	ErrInvalidStatsStation = errors.New("admin: invalid statistics station filter")
)

// RevenueQuery is a validated revenue request.
type RevenueQuery struct {
	// FromAt and ToAt are UTC Unix seconds. The range is half-open: FromAt is
	// included, ToAt is not.
	FromAt int64
	ToAt   int64
	// StationID filters to one station; 0 means every station.
	StationID int64
	// Bucket is StatsBucketHour or StatsBucketDay.
	Bucket string
}

// BucketSeconds is the width of one bucket in seconds.
func (q RevenueQuery) BucketSeconds() int64 {
	if q.Bucket == StatsBucketHour {
		return secondsPerHour
	}
	return secondsPerDay
}

// Validate reports whether the query is one the platform will run.
func (q RevenueQuery) Validate() error {
	if q.FromAt < 0 || q.ToAt <= q.FromAt {
		return fmt.Errorf("%w: FromAt must be non-negative and ToAt must be after it", ErrInvalidStatsRange)
	}
	if q.ToAt-q.FromAt > MaxStatsRangeSeconds {
		return fmt.Errorf("%w: the range may not exceed %d days", ErrInvalidStatsRange, MaxStatsRangeSeconds/secondsPerDay)
	}
	if q.Bucket != StatsBucketHour && q.Bucket != StatsBucketDay {
		return fmt.Errorf("%w: %q is not %s or %s", ErrInvalidStatsBucket, q.Bucket, StatsBucketHour, StatsBucketDay)
	}
	if q.StationID < 0 {
		return fmt.Errorf("%w: StationID must not be negative", ErrInvalidStatsStation)
	}
	return nil
}

// RevenueBucket is one point of the revenue series.
type RevenueBucket struct {
	// BucketStart is the UTC Unix second the bucket begins at.
	BucketStart int64
	AmountCent  int64
	EnergyMwh   int64
	OrderCount  int64
}

// RevenueTotals is the whole range, which is not always the sum of the buckets
// a caller receives: the buckets are capped, the totals are not.
type RevenueTotals struct {
	AmountCent int64
	EnergyMwh  int64
	OrderCount int64
}

// RevenueStats is the revenue series with its range totals.
type RevenueStats struct {
	Items           []RevenueBucket
	TotalAmountCent int64
	TotalEnergyMwh  int64
	TotalOrderCount int64
}

// ChargerCounts is the raw fleet census, as the store reports it.
type ChargerCounts struct {
	Idle       int64
	Occupied   int64
	Faulty     int64
	Restarting int64
	Disabled   int64
	Total      int64
}

// ChargerStatusStats is the fleet census with the derived health figures.
type ChargerStatusStats struct {
	IdleCount        int64
	OccupiedCount    int64
	FaultyCount      int64
	RestartingCount  int64
	DisabledCount    int64
	OperationalCount int64
	TotalCount       int64
	// HealthPercent is OperationalCount as a percentage of TotalCount, rounded
	// to two decimals; 0 when the fleet is empty.
	HealthPercent float64
}

// FleetCounts is the lifetime and current-state census behind the overview.
type FleetCounts struct {
	// TotalRevenueCent, CompletedOrders, FastOrders and SlowOrders are lifetime
	// figures over every completed order, not over a window.
	TotalRevenueCent int64
	CompletedOrders  int64
	FastOrders       int64
	SlowOrders       int64
	// RegisteredUsers counts every account the platform holds.
	RegisteredUsers int64
	// Stations counts every station, including a disabled one: an operator
	// asking how many stations exist is not asking how many are open.
	Stations int64
	// ActiveOrders counts the orders that have not reached a terminal state.
	ActiveOrders int64
}

// StatsOverview is the operations snapshot behind GET /admin/stats/overview.
//
// It is the scalar block the old dashboard snapshot carried: what the platform
// has, not what happened in a window. Windowed figures belong to
// GET /admin/stats/revenue, because only the client knows which timezone its
// "today" is in.
type StatsOverview struct {
	GeneratedAt         int64
	TotalRevenueCent    int64
	TotalChargeCount    int64
	FastChargeCount     int64
	SlowChargeCount     int64
	RegisteredUserCount int64
	StationCount        int64
	ActiveOrderCount    int64
	Chargers            ChargerStatusStats
}

// RevenueStats returns the revenue series and the range totals.
//
// The series is dense: every bucket in the range is present, with zeros where
// nothing happened. A chart that skipped the quiet days would draw a misleading
// trend, and a table that omitted them would look like missing data.
func (s *Service) RevenueStats(ctx context.Context, query RevenueQuery) (RevenueStats, error) {
	if err := query.Validate(); err != nil {
		return RevenueStats{}, err
	}
	rows, err := s.store.RevenueBuckets(ctx, query)
	if err != nil {
		return RevenueStats{}, err
	}
	totals, err := s.store.RevenueTotals(ctx, query)
	if err != nil {
		return RevenueStats{}, err
	}
	return RevenueStats{
		Items:           fillRevenueBuckets(query, rows),
		TotalAmountCent: totals.AmountCent,
		TotalEnergyMwh:  totals.EnergyMwh,
		TotalOrderCount: totals.OrderCount,
	}, nil
}

// ChargerStatus returns the fleet census with its derived health figures.
func (s *Service) ChargerStatus(ctx context.Context, stationID int64) (ChargerStatusStats, error) {
	if stationID < 0 {
		return ChargerStatusStats{}, fmt.Errorf("%w: station id must not be negative", ErrInvalidStatsStation)
	}
	counts, err := s.store.ChargerCounts(ctx, stationID)
	if err != nil {
		return ChargerStatusStats{}, err
	}
	return newChargerStatusStats(counts), nil
}

// Overview returns the operations snapshot.
func (s *Service) Overview(ctx context.Context) (StatsOverview, error) {
	counts, err := s.store.FleetCounts(ctx)
	if err != nil {
		return StatsOverview{}, err
	}
	chargers, err := s.store.ChargerCounts(ctx, 0)
	if err != nil {
		return StatsOverview{}, err
	}
	return StatsOverview{
		GeneratedAt:         s.clock().Unix(),
		TotalRevenueCent:    counts.TotalRevenueCent,
		TotalChargeCount:    counts.CompletedOrders,
		FastChargeCount:     counts.FastOrders,
		SlowChargeCount:     counts.SlowOrders,
		RegisteredUserCount: counts.RegisteredUsers,
		StationCount:        counts.Stations,
		ActiveOrderCount:    counts.ActiveOrders,
		Chargers:            newChargerStatusStats(chargers),
	}, nil
}

// newChargerStatusStats derives the operational count and the health figure.
//
// The rule is the old service's: idle and occupied devices are operational, the
// rest are not. The percentage keeps two decimals where the old one truncated
// to a whole number - the console shows a fraction of a percent, and throwing
// it away made a fleet that lost two devices look unchanged.
func newChargerStatusStats(counts ChargerCounts) ChargerStatusStats {
	operational := counts.Idle + counts.Occupied
	health := 0.0
	if counts.Total > 0 {
		health = math.Round(float64(operational)*10000/float64(counts.Total)) / 100
	}
	return ChargerStatusStats{
		IdleCount:        counts.Idle,
		OccupiedCount:    counts.Occupied,
		FaultyCount:      counts.Faulty,
		RestartingCount:  counts.Restarting,
		DisabledCount:    counts.Disabled,
		OperationalCount: operational,
		TotalCount:       counts.Total,
		HealthPercent:    health,
	}
}

// fillRevenueBuckets lays the stored buckets onto a dense series.
//
// Buckets are aligned to FromAt rather than to the epoch. The client computes
// its own day boundaries - its "today" starts at its local midnight - and asks
// for exactly that range, so aligning to the range is what makes each bucket
// one of the client's days. Epoch alignment would put the first bucket's
// boundary in the middle of a local day and show a partial day as a quiet one.
func fillRevenueBuckets(query RevenueQuery, rows []RevenueBucket) []RevenueBucket {
	width := query.BucketSeconds()
	stored := make(map[int64]RevenueBucket, len(rows))
	for _, row := range rows {
		stored[row.BucketStart] = row
	}

	items := make([]RevenueBucket, 0, (query.ToAt-query.FromAt)/width+1)
	for start := query.FromAt; start < query.ToAt; start += width {
		bucket, found := stored[start]
		if !found {
			bucket = RevenueBucket{}
		}
		bucket.BucketStart = start
		items = append(items, bucket)
	}
	return items
}
