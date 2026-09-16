package admin

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/heguangV/charging-station-platform/backend/internal/auth"
)

// The statistics boundary: what a query may be, how a series is laid out, and
// what the console receives. The SQL itself is exercised by the PostgreSQL
// integration test beside the store.

func getStats(t *testing.T, f fixture, path string) *httptest.ResponseRecorder {
	t.Helper()
	request := httptest.NewRequest(http.MethodGet, path, nil)
	recorder := httptest.NewRecorder()
	f.server.Handler().ServeHTTP(recorder, request)
	return recorder
}

func decodeStats(t *testing.T, recorder *httptest.ResponseRecorder) map[string]any {
	t.Helper()
	var envelope struct {
		Success bool            `json:"success"`
		Code    int             `json:"code"`
		Message string          `json:"message"`
		Data    json.RawMessage `json:"data"`
	}
	if err := json.Unmarshal(recorder.Body.Bytes(), &envelope); err != nil {
		t.Fatalf("the response is not an envelope: %s", recorder.Body.String())
	}
	if !envelope.Success || envelope.Code != 0 {
		t.Fatalf("envelope = %+v", envelope)
	}
	payload := map[string]any{}
	if err := json.Unmarshal(envelope.Data, &payload); err != nil {
		t.Fatalf("the payload is not an object: %s", envelope.Data)
	}
	return payload
}

// —— the query contract ——

// The range is required and bounded, the bucket is an enum, and the station
// filter is a positive id. Each of these is a query the platform will not run,
// so each is refused before it reaches SQL.
func TestRevenueQueryValidation(t *testing.T) {
	valid := RevenueQuery{FromAt: 1000, ToAt: 1000 + 3600, Bucket: StatsBucketHour}
	if err := valid.Validate(); err != nil {
		t.Fatalf("a well-formed query was refused: %v", err)
	}
	if got := valid.BucketSeconds(); got != 3600 {
		t.Fatalf("hour bucket = %ds, want 3600", got)
	}
	if got := (RevenueQuery{Bucket: StatsBucketDay}).BucketSeconds(); got != 86400 {
		t.Fatalf("day bucket = %ds, want 86400", got)
	}

	cases := map[string]struct {
		query RevenueQuery
		want  error
	}{
		"negative start":   {RevenueQuery{FromAt: -1, ToAt: 10, Bucket: StatsBucketDay}, ErrInvalidStatsRange},
		"inverted range":   {RevenueQuery{FromAt: 100, ToAt: 100, Bucket: StatsBucketDay}, ErrInvalidStatsRange},
		"reversed range":   {RevenueQuery{FromAt: 200, ToAt: 100, Bucket: StatsBucketDay}, ErrInvalidStatsRange},
		"range too long":   {RevenueQuery{FromAt: 0, ToAt: MaxStatsRangeSeconds + 1, Bucket: StatsBucketDay}, ErrInvalidStatsRange},
		"unknown bucket":   {RevenueQuery{FromAt: 0, ToAt: 3600, Bucket: "week"}, ErrInvalidStatsBucket},
		"empty bucket":     {RevenueQuery{FromAt: 0, ToAt: 3600}, ErrInvalidStatsBucket},
		"negative station": {RevenueQuery{FromAt: 0, ToAt: 3600, Bucket: StatsBucketDay, StationID: -1}, ErrInvalidStatsStation},
	}
	for name, tc := range cases {
		t.Run(name, func(t *testing.T) {
			err := tc.query.Validate()
			if !errors.Is(err, tc.want) {
				t.Fatalf("Validate() = %v, want %v", err, tc.want)
			}
		})
	}
}

// The maximum range is a bound, not an approximation: exactly 90 days is
// allowed.
func TestRevenueQueryAcceptsTheMaximumRange(t *testing.T) {
	query := RevenueQuery{FromAt: 0, ToAt: MaxStatsRangeSeconds, Bucket: StatsBucketDay}
	if err := query.Validate(); err != nil {
		t.Fatalf("the maximum range was refused: %v", err)
	}
}

// —— the series ——

// A quiet day is part of the trend. Omitting it would draw a chart that
// connects two busy days as if nothing happened between them.
func TestRevenueSeriesIsDense(t *testing.T) {
	// Four hours, of which the second and the fourth had orders. The requested
	// start is deliberately not an epoch boundary: the buckets have to follow
	// the range the client asked for, not the clock.
	const fromAt = int64(1788134400)
	store := &fakeStore{
		revenueTotals: RevenueTotals{AmountCent: 5000, EnergyMwh: 9000, OrderCount: 3},
		revenueBuckets: []RevenueBucket{
			{BucketStart: fromAt + 3600, AmountCent: 2000, EnergyMwh: 3000, OrderCount: 2},
			{BucketStart: fromAt + 3*3600, AmountCent: 3000, EnergyMwh: 6000, OrderCount: 1},
		},
	}
	service, err := NewService(store)
	if err != nil {
		t.Fatalf("NewService() error = %v", err)
	}

	stats, err := service.RevenueStats(context.Background(),
		RevenueQuery{FromAt: fromAt, ToAt: fromAt + 4*3600, Bucket: StatsBucketHour})
	if err != nil {
		t.Fatalf("RevenueStats() error = %v", err)
	}

	want := []int64{fromAt, fromAt + 3600, fromAt + 2*3600, fromAt + 3*3600}
	if len(stats.Items) != len(want) {
		t.Fatalf("items = %d, want %d", len(stats.Items), len(want))
	}
	for index, start := range want {
		if stats.Items[index].BucketStart != start {
			t.Fatalf("item %d starts at %d, want %d", index, stats.Items[index].BucketStart, start)
		}
	}
	if stats.Items[0].OrderCount != 0 || stats.Items[2].OrderCount != 0 {
		t.Fatalf("a quiet bucket must be present and zero: %+v", stats.Items)
	}
	if stats.Items[1].AmountCent != 2000 || stats.Items[3].AmountCent != 3000 {
		t.Fatalf("the stored buckets were not merged: %+v", stats.Items)
	}
	// The totals are the store's own, not a sum of the series: the series is a
	// window onto the same data and the two must not be recomputed differently.
	if stats.TotalAmountCent != 5000 || stats.TotalEnergyMwh != 9000 || stats.TotalOrderCount != 3 {
		t.Fatalf("totals = %+v", stats)
	}
}

// The station filter and the bucket reach the store unchanged.
func TestRevenueQueryReachesTheStore(t *testing.T) {
	store := &fakeStore{}
	service, err := NewService(store)
	if err != nil {
		t.Fatalf("NewService() error = %v", err)
	}

	if _, err := service.RevenueStats(context.Background(),
		RevenueQuery{FromAt: 1000, ToAt: 5000, Bucket: StatsBucketHour, StationID: 7}); err != nil {
		t.Fatalf("RevenueStats() error = %v", err)
	}
	if len(store.revenueQueries) != 2 {
		t.Fatalf("queries = %d, want one totals and one buckets call", len(store.revenueQueries))
	}
	for _, query := range store.revenueQueries {
		if query.StationID != 7 || query.Bucket != StatsBucketHour || query.FromAt != 1000 || query.ToAt != 5000 {
			t.Fatalf("the store received %+v", query)
		}
	}
}

// A refused query must not reach the store at all.
func TestRevenueStatsRefusesBeforeQuerying(t *testing.T) {
	store := &fakeStore{}
	service, err := NewService(store)
	if err != nil {
		t.Fatalf("NewService() error = %v", err)
	}
	if _, err := service.RevenueStats(context.Background(), RevenueQuery{FromAt: 10, ToAt: 20, Bucket: "week"}); err == nil {
		t.Fatal("expected the bucket to be refused")
	}
	if len(store.revenueQueries) != 0 {
		t.Fatalf("a refused query reached the store: %+v", store.revenueQueries)
	}
}

// An empty range still describes every bucket in it.
func TestRevenueSeriesOfAnEmptyRange(t *testing.T) {
	store := &fakeStore{}
	service, err := NewService(store)
	if err != nil {
		t.Fatalf("NewService() error = %v", err)
	}
	stats, err := service.RevenueStats(context.Background(),
		RevenueQuery{FromAt: 0, ToAt: 3 * 3600, Bucket: StatsBucketHour})
	if err != nil {
		t.Fatalf("RevenueStats() error = %v", err)
	}
	if len(stats.Items) != 3 {
		t.Fatalf("items = %d, want 3 empty hours", len(stats.Items))
	}
	for _, item := range stats.Items {
		if item.AmountCent != 0 || item.OrderCount != 0 {
			t.Fatalf("an empty range produced a non-zero bucket: %+v", item)
		}
	}
}

// —— the fleet census ——

// The health rule is the old service's: idle and occupied devices are
// operational, and the rest are not. The fraction is kept, because rounding a
// fleet's health to a whole percent hides the loss of a single device.
func TestChargerStatusDerivesHealth(t *testing.T) {
	store := &fakeStore{chargerCounts: ChargerCounts{
		Idle: 12, Occupied: 5, Faulty: 2, Restarting: 1, Disabled: 4, Total: 24,
	}}
	service, err := NewService(store)
	if err != nil {
		t.Fatalf("NewService() error = %v", err)
	}

	stats, err := service.ChargerStatus(context.Background(), 0)
	if err != nil {
		t.Fatalf("ChargerStatus() error = %v", err)
	}

	if stats.OperationalCount != 17 {
		t.Fatalf("operational = %d, want idle+occupied = 17", stats.OperationalCount)
	}
	if stats.TotalCount != 24 {
		t.Fatalf("total = %d, want 24", stats.TotalCount)
	}
	// 17/24 rounds to 70.83, the figure the console's own fixture pins.
	if stats.HealthPercent != 70.83 {
		t.Fatalf("health = %v, want 70.83", stats.HealthPercent)
	}
	if stats.IdleCount != 12 || stats.OccupiedCount != 5 || stats.FaultyCount != 2 ||
		stats.RestartingCount != 1 || stats.DisabledCount != 4 {
		t.Fatalf("the census was not carried through: %+v", stats)
	}
}

// An empty fleet has no health to report, and must not divide by zero.
func TestChargerStatusOfAnEmptyFleet(t *testing.T) {
	store := &fakeStore{}
	service, err := NewService(store)
	if err != nil {
		t.Fatalf("NewService() error = %v", err)
	}
	stats, err := service.ChargerStatus(context.Background(), 0)
	if err != nil {
		t.Fatalf("ChargerStatus() error = %v", err)
	}
	if stats.TotalCount != 0 || stats.OperationalCount != 0 || stats.HealthPercent != 0 {
		t.Fatalf("an empty fleet reported %+v", stats)
	}
}

// Health is rounded to two decimals rather than truncated: a fleet at 99.6%
// must not read as 99%.
func TestChargerHealthRoundsRatherThanTruncates(t *testing.T) {
	stats := newChargerStatusStats(ChargerCounts{Idle: 499, Occupied: 0, Faulty: 1, Total: 500})
	if stats.HealthPercent != 99.8 {
		t.Fatalf("health = %v, want 99.8", stats.HealthPercent)
	}
}

// A negative station filter is a query the platform will not run.
func TestChargerStatusRejectsANegativeStation(t *testing.T) {
	service, err := NewService(&fakeStore{})
	if err != nil {
		t.Fatalf("NewService() error = %v", err)
	}
	if _, err := service.ChargerStatus(context.Background(), -1); !errors.Is(err, ErrInvalidStatsStation) {
		t.Fatalf("error = %v, want ErrInvalidStatsStation", err)
	}
}

// —— the overview ——

// The overview is the old snapshot's scalar block: what the platform has, not
// what happened in a window.
func TestOverviewComposesTheSnapshot(t *testing.T) {
	store := &fakeStore{
		fleetCounts: FleetCounts{
			TotalRevenueCent: 123456, CompletedOrders: 42, FastOrders: 30, SlowOrders: 12,
			RegisteredUsers: 88, Stations: 6, ActiveOrders: 2,
		},
		chargerCounts: ChargerCounts{Idle: 3, Occupied: 1, Faulty: 1, Total: 5},
	}
	service, err := NewService(store)
	if err != nil {
		t.Fatalf("NewService() error = %v", err)
	}

	overview, err := service.Overview(context.Background())
	if err != nil {
		t.Fatalf("Overview() error = %v", err)
	}

	if overview.TotalRevenueCent != 123456 || overview.TotalChargeCount != 42 {
		t.Fatalf("overview = %+v", overview)
	}
	if overview.FastChargeCount != 30 || overview.SlowChargeCount != 12 {
		t.Fatalf("the charger mix was not carried through: %+v", overview)
	}
	if overview.RegisteredUserCount != 88 || overview.StationCount != 6 || overview.ActiveOrderCount != 2 {
		t.Fatalf("the census was not carried through: %+v", overview)
	}
	if overview.Chargers.OperationalCount != 4 || overview.Chargers.HealthPercent != 80 {
		t.Fatalf("chargers = %+v", overview.Chargers)
	}
	if overview.GeneratedAt <= 0 {
		t.Fatalf("generatedAt = %d, want a timestamp", overview.GeneratedAt)
	}
}

// —— the endpoints ——

// The three paths are published and served, and every one of them needs an admin
// session: these are the platform's revenue and fleet figures.
func TestStatsEndpointsRequireAnAdminSession(t *testing.T) {
	paths := []string{
		RevenueStatsPath + "?fromAt=0&toAt=86400",
		ChargerStatsPath,
		OverviewStatsPath,
	}
	for _, path := range paths {
		t.Run(path, func(t *testing.T) {
			anonymous := newFixture(t, auth.Identity{}, false)
			if recorder := getStats(t, anonymous, path); recorder.Code != http.StatusUnauthorized {
				t.Fatalf("status = %d, want 401 without a session", recorder.Code)
			}

			// A user session is not an administrator session.
			user := newFixture(t, auth.Identity{ID: 5, Role: auth.RoleUser}, true)
			if recorder := getStats(t, user, path); recorder.Code != http.StatusForbidden {
				t.Fatalf("status = %d, want 403 for a user session", recorder.Code)
			}
		})
	}
}

// Every statistics route is a read.
func TestStatsEndpointsRejectOtherMethods(t *testing.T) {
	for _, path := range []string{RevenueStatsPath, ChargerStatsPath, OverviewStatsPath} {
		t.Run(path, func(t *testing.T) {
			f := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
			request := httptest.NewRequest(http.MethodPost, path, strings.NewReader("{}"))
			recorder := httptest.NewRecorder()
			f.server.Handler().ServeHTTP(recorder, request)
			if recorder.Code != http.StatusMethodNotAllowed {
				t.Fatalf("status = %d, want 405", recorder.Code)
			}
			if allow := recorder.Header().Get("Allow"); allow != http.MethodGet {
				t.Fatalf("Allow = %q, want GET", allow)
			}
		})
	}
}

// The parameters are checked at the boundary, and a refused request does no
// work.
func TestRevenueEndpointValidatesItsQuery(t *testing.T) {
	cases := map[string]string{
		"missing fromAt":   "?toAt=86400",
		"missing toAt":     "?fromAt=0",
		"non-numeric":      "?fromAt=abc&toAt=86400",
		"inverted":         "?fromAt=86400&toAt=0",
		"equal":            "?fromAt=100&toAt=100",
		"range too long":   fmt.Sprintf("?fromAt=0&toAt=%d", MaxStatsRangeSeconds+1),
		"unknown bucket":   "?fromAt=0&toAt=86400&bucket=week",
		"zero station":     "?fromAt=0&toAt=86400&stationId=0",
		"negative station": "?fromAt=0&toAt=86400&stationId=-3",
	}
	for name, query := range cases {
		t.Run(name, func(t *testing.T) {
			f := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
			recorder := getStats(t, f, RevenueStatsPath+query)
			if recorder.Code != http.StatusBadRequest {
				t.Fatalf("status = %d, want 400 (%s)", recorder.Code, recorder.Body.String())
			}
			if len(f.store.revenueQueries) != 0 {
				t.Fatalf("a refused query reached the store: %+v", f.store.revenueQueries)
			}
		})
	}
}

// The revenue payload is exactly what the console's parser requires.
func TestRevenueEndpointResponseShape(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	f.store.revenueTotals = RevenueTotals{AmountCent: 123456, EnergyMwh: 1500000, OrderCount: 12}
	f.store.revenueBuckets = []RevenueBucket{
		{BucketStart: 1788134400, AmountCent: 123456, EnergyMwh: 1500000, OrderCount: 12},
	}

	recorder := getStats(t, f, RevenueStatsPath+"?fromAt=1788134400&toAt=1788220800&bucket=day")
	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d (%s)", recorder.Code, recorder.Body.String())
	}
	payload := decodeStats(t, recorder)

	want := []string{"items", "totalAmountCent", "totalEnergyMwh", "totalOrderCount"}
	if got := sortedKeys(payload); strings.Join(got, ",") != strings.Join(want, ",") {
		t.Fatalf("keys = %v, want %v", got, want)
	}
	items, isList := payload["items"].([]any)
	if !isList || len(items) != 1 {
		t.Fatalf("items = %#v, want one bucket", payload["items"])
	}
	item, _ := items[0].(map[string]any)
	itemKeys := []string{"amountCent", "bucketStart", "energyMwh", "orderCount"}
	if got := sortedKeys(item); strings.Join(got, ",") != strings.Join(itemKeys, ",") {
		t.Fatalf("bucket keys = %v, want %v", got, itemKeys)
	}
	if payload["totalAmountCent"] != float64(123456) || payload["totalEnergyMwh"] != float64(1500000) {
		t.Fatalf("totals = %#v", payload)
	}
}

// An empty range still sends a list, never null: the console iterates it
// directly.
func TestRevenueEndpointSendsAListWhenEmpty(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	recorder := getStats(t, f, RevenueStatsPath+"?fromAt=0&toAt=3600&bucket=hour")
	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d", recorder.Code)
	}
	payload := decodeStats(t, recorder)
	items, isList := payload["items"].([]any)
	if !isList {
		t.Fatalf("items = %#v, want a list", payload["items"])
	}
	if len(items) != 1 {
		t.Fatalf("items = %d, want the one empty hour", len(items))
	}
}

// The charger payload is the shape the breakdown chart reads.
func TestChargerStatsEndpointResponseShape(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	f.store.chargerCounts = ChargerCounts{Idle: 12, Occupied: 5, Faulty: 2, Restarting: 1, Disabled: 4, Total: 24}

	recorder := getStats(t, f, ChargerStatsPath)
	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d (%s)", recorder.Code, recorder.Body.String())
	}
	payload := decodeStats(t, recorder)

	want := []string{"disabledCount", "faultyCount", "healthPercent", "idleCount",
		"occupiedCount", "operationalCount", "restartingCount", "totalCount"}
	if got := sortedKeys(payload); strings.Join(got, ",") != strings.Join(want, ",") {
		t.Fatalf("keys = %v, want %v", got, want)
	}
	if payload["healthPercent"] != 70.83 {
		t.Fatalf("healthPercent = %#v, want 70.83", payload["healthPercent"])
	}
	// The station filter is optional and reaches the store as "every station".
	if len(f.store.stationFilters) != 1 || f.store.stationFilters[0] != 0 {
		t.Fatalf("station filters = %v, want one unfiltered query", f.store.stationFilters)
	}

	filtered := getStats(t, f, ChargerStatsPath+"?stationId=9")
	if filtered.Code != http.StatusOK {
		t.Fatalf("filtered status = %d", filtered.Code)
	}
	if f.store.stationFilters[len(f.store.stationFilters)-1] != 9 {
		t.Fatalf("station filters = %v, want the requested station", f.store.stationFilters)
	}
}

// The overview payload carries the snapshot's scalar block and the fleet
// breakdown.
func TestOverviewEndpointResponseShape(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	f.store.fleetCounts = FleetCounts{TotalRevenueCent: 999, CompletedOrders: 4, FastOrders: 3, SlowOrders: 1,
		RegisteredUsers: 20, Stations: 2, ActiveOrders: 1}
	f.store.chargerCounts = ChargerCounts{Idle: 1, Total: 1}

	recorder := getStats(t, f, OverviewStatsPath)
	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d (%s)", recorder.Code, recorder.Body.String())
	}
	payload := decodeStats(t, recorder)

	want := []string{"activeOrderCount", "chargers", "fastChargeCount", "generatedAt",
		"registeredUserCount", "slowChargeCount", "stationCount", "totalChargeCount", "totalRevenueCent"}
	if got := sortedKeys(payload); strings.Join(got, ",") != strings.Join(want, ",") {
		t.Fatalf("keys = %v, want %v", got, want)
	}
	chargers, isObject := payload["chargers"].(map[string]any)
	if !isObject {
		t.Fatalf("chargers = %#v, want an object", payload["chargers"])
	}
	if chargers["healthPercent"] != float64(100) {
		t.Fatalf("chargers = %#v", chargers)
	}
}

// A store failure is a service problem, not a caller mistake: the console must
// be told to retry rather than to fix its query.
func TestStatsEndpointReportsAStoreFailureAsUnavailable(t *testing.T) {
	f := newFixture(t, adminIdentity(auth.AdminRoleOperator), true)
	f.store.statsErr = errors.New("the database is unreachable")

	recorder := getStats(t, f, ChargerStatsPath)
	if recorder.Code != http.StatusServiceUnavailable {
		t.Fatalf("status = %d, want 503 (%s)", recorder.Code, recorder.Body.String())
	}
}
