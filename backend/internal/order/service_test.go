package order

import (
	"strings"
	"testing"
	"time"

	"github.com/heguangV/charging-station-platform/backend/internal/event"
)

func TestCanTransitionCoversP0Machine(t *testing.T) {
	valid := map[string][]string{
		StatusCreated:  {StatusStarting, StatusCancelled, StatusExpired, StatusFailed},
		StatusStarting: {StatusCharging, StatusCancelled, StatusFailed},
		StatusCharging: {StatusStopping, StatusFailed},
		StatusStopping: {StatusCompleted, StatusFailed},
	}
	for from, targets := range valid {
		for _, to := range targets {
			if !CanTransition(from, to) {
				t.Errorf("CanTransition(%s, %s) = false, want true", from, to)
			}
		}
	}

	invalid := [][2]string{
		{StatusCreated, StatusCharging},   // must pass through STARTING
		{StatusCreated, StatusCompleted},  // must pass through the whole chain
		{StatusStarting, StatusStopping},  // device stop without start confirmation
		{StatusCharging, StatusCharging},  // no self transition
		{StatusCompleted, StatusCharging}, // terminal
		{StatusCancelled, StatusStarting}, // terminal
		{StatusExpired, StatusStarting},   // terminal
		{StatusFailed, StatusCreated},     // terminal
		{StatusStopping, StatusStopping},  // no self transition
		{"UNKNOWN", StatusStarting},       // unknown source
		{StatusCreated, "UNKNOWN"},        // unknown target
	}
	for _, pair := range invalid {
		if CanTransition(pair[0], pair[1]) {
			t.Errorf("CanTransition(%s, %s) = true, want false", pair[0], pair[1])
		}
		if err := ValidateTransition(pair[0], pair[1]); err == nil {
			t.Errorf("ValidateTransition(%s, %s) = nil, want error", pair[0], pair[1])
		}
	}
}

func TestValidateTransitionWrapsSentinel(t *testing.T) {
	err := ValidateTransition(StatusCharging, StatusCompleted)
	if err == nil || !strings.Contains(err.Error(), StatusCharging) || !strings.Contains(err.Error(), StatusCompleted) {
		t.Fatalf("ValidateTransition error = %v, want message with both statuses", err)
	}
}

func TestAmountCentsRoundsHalfUp(t *testing.T) {
	cases := []struct {
		energyWh int64
		price    int64
		want     int64
	}{
		{0, 100, 0},
		{1500, 100, 150},         // 1.5 kWh * 1元 = 1.50元
		{1000, 120, 120},         // exact
		{1, 100, 0},              // 0.1分 rounds down
		{5, 100, 1},              // 0.5分 rounds up (half up)
		{733, 87, 64},            // 63.771 → 64
		{100000, 10000, 1000000}, // 100kWh * 100元/kWh
	}
	for _, testCase := range cases {
		if got := AmountCents(testCase.energyWh, testCase.price); got != testCase.want {
			t.Errorf("AmountCents(%d, %d) = %d, want %d", testCase.energyWh, testCase.price, got, testCase.want)
		}
	}
	if AmountCents(-5, 100) != 0 {
		t.Error("negative energy must clamp to zero")
	}
	if AmountCents(100, -5) != 0 {
		t.Error("negative price must clamp to zero")
	}
}

func TestNewOrderNoFormatAndUniqueness(t *testing.T) {
	now := time.Date(2026, 9, 14, 12, 0, 0, 0, time.UTC)
	first, err := NewOrderNo(now)
	if err != nil {
		t.Fatalf("NewOrderNo() error = %v", err)
	}
	if !strings.HasPrefix(first, "ORD20260914120000") || len(first) != 25 {
		t.Fatalf("order no = %q, want ORD+timestamp+8 hex", first)
	}
	second, err := NewOrderNo(now)
	if err != nil {
		t.Fatalf("NewOrderNo() error = %v", err)
	}
	if first == second {
		t.Fatal("two order numbers at the same instant are identical")
	}
}

func TestNewEventIDFormat(t *testing.T) {
	id, err := NewEventID()
	if err != nil {
		t.Fatalf("NewEventID() error = %v", err)
	}
	if !strings.HasPrefix(id, "evt_") || len(id) != len("evt_")+32 {
		t.Fatalf("event id = %q, want evt_ + 32 hex chars", id)
	}
}

// TestEventTypesMatchBLineContract pins the A-line event constants against the
// merged B-line event package so the two lines cannot drift apart silently.
func TestEventTypesMatchBLineContract(t *testing.T) {
	cases := []struct {
		ours   string
		theirs event.Type
	}{
		{EventOrderCreated, event.OrderCreated},
		{EventChargeStartRequested, event.ChargeStartRequested},
		{EventChargeStarted, event.ChargeStarted},
		{EventChargeStopRequested, event.ChargeStopRequested},
		{EventChargeStopped, event.ChargeStopped},
		{EventOrderCompleted, event.OrderCompleted},
	}
	for _, testCase := range cases {
		if testCase.ours != string(testCase.theirs) {
			t.Errorf("event type drift: %q != B-line %q", testCase.ours, testCase.theirs)
		}
	}
}

func TestIsActiveAndIsValidStatus(t *testing.T) {
	for _, status := range ActiveStatuses {
		if !IsActive(status) {
			t.Errorf("IsActive(%s) = false", status)
		}
	}
	for _, status := range []string{StatusCompleted, StatusCancelled, StatusExpired, StatusFailed} {
		if IsActive(status) {
			t.Errorf("IsActive(%s) = true, want false", status)
		}
	}
	if IsValidStatus("AVAILABLE") || IsValidStatus("") {
		t.Error("invalid statuses accepted")
	}
}

func TestNewServiceRequiresStore(t *testing.T) {
	if _, err := NewService(nil); err == nil {
		t.Fatal("nil store accepted")
	}
}

func TestEffectiveElectricityPriceResolvesTimeOfUse(t *testing.T) {
	offPeak := int64(60)
	windowStart := int16(23)
	windowEnd := int16(7) // window spans midnight
	cfg := TariffConfig{PeakPrice: 120, OffPeak: &offPeak, WindowStart: &windowStart, WindowEnd: &windowEnd}

	at := func(hour int) time.Time {
		return time.Date(2026, 9, 15, hour, 0, 0, 0, time.UTC)
	}

	if got := EffectiveElectricityPrice(at(14), cfg); got != 120 {
		t.Fatalf("afternoon price = %d, want peak 120", got)
	}
	if got := EffectiveElectricityPrice(at(2), cfg); got != 60 {
		t.Fatalf("night price = %d, want off-peak 60", got)
	}

	// No off-peak configuration: always peak.
	if got := EffectiveElectricityPrice(at(2), TariffConfig{PeakPrice: 120}); got != 120 {
		t.Fatalf("no-tou price = %d, want peak 120", got)
	}
}

// The window is read in the location of the instant it is given, so the same UTC instant can be
// peak or off-peak depending on the billing timezone. That is the whole reason the store converts
// into NCS_BILLING_TZ before billing: an operator's 23:00-07:00 means local night.
func TestTariffWindowFollowsTheBillingLocation(t *testing.T) {
	offPeak := int64(60)
	windowStart := int16(23)
	windowEnd := int16(7)
	cfg := TariffConfig{PeakPrice: 120, OffPeak: &offPeak, WindowStart: &windowStart, WindowEnd: &windowEnd}

	// 04:00 UTC is 12:00 in +08.
	instant := time.Date(2026, 9, 15, 4, 0, 0, 0, time.UTC)

	if got := EffectiveElectricityPrice(instant, cfg); got != 60 {
		t.Fatalf("04:00 UTC price = %d, want off-peak 60 when the window is read in UTC", got)
	}
	if got := EffectiveElectricityPrice(instant.In(DefaultBillingLocation()), cfg); got != 120 {
		t.Fatalf("04:00 UTC (=12:00 local) price = %d, want peak 120 when the window is read in +08", got)
	}
}

func TestDefaultBillingLocationIsChinaStandardTime(t *testing.T) {
	_, offset := time.Date(2026, 9, 15, 0, 0, 0, 0, DefaultBillingLocation()).Zone()
	if offset != 8*60*60 {
		t.Fatalf("default billing offset = %d seconds, want +08 (an operator's off-peak window is local night)", offset)
	}
}

func TestUnitPriceCentsAddsTheServiceFee(t *testing.T) {
	offPeak := int64(60)
	windowStart := int16(23)
	windowEnd := int16(7)
	cfg := TariffConfig{PeakPrice: 120, OffPeak: &offPeak, WindowStart: &windowStart, WindowEnd: &windowEnd, ServicePrice: 50}
	billing := DefaultBillingLocation()

	if got := UnitPriceCents(time.Date(2026, 9, 15, 12, 0, 0, 0, billing), cfg); got != 170 {
		t.Fatalf("midday unit price = %d, want 170 (120 peak + 50 service)", got)
	}
	if got := UnitPriceCents(time.Date(2026, 9, 15, 23, 30, 0, 0, billing), cfg); got != 110 {
		t.Fatalf("night unit price = %d, want 110 (60 off-peak + 50 service)", got)
	}
	if got := UnitPriceCents(time.Date(2026, 9, 15, 12, 0, 0, 0, billing), TariffConfig{PeakPrice: 100}); got != 100 {
		t.Fatalf("flat unit price = %d, want 100", got)
	}
}

func TestComputeTOUBillSplitsEnergyAcrossSegments(t *testing.T) {
	offPeak := int64(60)
	windowStart := int16(23)
	windowEnd := int16(7)
	cfg := TariffConfig{PeakPrice: 120, OffPeak: &offPeak, WindowStart: &windowStart, WindowEnd: &windowEnd, ServicePrice: 50}

	// A one-hour charge crossing the 23:00 boundary: half peak, half off-peak.
	start := time.Date(2026, 9, 15, 22, 30, 0, 0, time.UTC)
	end := time.Date(2026, 9, 15, 23, 30, 0, 0, time.UTC)
	bill := ComputeTOUBill(3600, start, end, cfg)

	if len(bill.Segments) != 2 {
		t.Fatalf("segments = %d, want 2", len(bill.Segments))
	}
	energySum := int64(0)
	feeSum := int64(0)
	for _, segment := range bill.Segments {
		energySum += segment.EnergyWh
		feeSum += segment.FeeCent
		if segment.EnergyWh != 1800 {
			t.Errorf("segment energy = %d, want 1800 (proportional split)", segment.EnergyWh)
		}
	}
	if energySum != 3600 {
		t.Fatalf("segment energies sum to %d, want 3600", energySum)
	}
	// Peak: 1.8kWh x 120 = 216 cents; off-peak: 1.8kWh x 60 = 108 cents.
	if feeSum != 216+108 {
		t.Fatalf("electricity fees = %d, want 324", feeSum)
	}
	if bill.AmountCent != feeSum+bill.ServiceFeeCent {
		t.Fatalf("amount %d != components %d+%d", bill.AmountCent, feeSum, bill.ServiceFeeCent)
	}
	if bill.ServiceFeeCent != 180 {
		t.Fatalf("service fee = %d, want 180 (1.8kWh x 50 x 2... see below)", bill.ServiceFeeCent)
	}
}

func TestComputeBillSplitsElectricityAndServiceFees(t *testing.T) {
	bill := ComputeBill(1500, 100, 50)
	if bill.ElectricityFeeCen != 150 || bill.ServiceFeeCent != 75 {
		t.Fatalf("component fees = %d/%d, want 150/75", bill.ElectricityFeeCen, bill.ServiceFeeCent)
	}
	if bill.AmountCent != 225 {
		t.Fatalf("amount = %d, want 225 (1.5kWh x 150 cents)", bill.AmountCent)
	}
}

func TestComputeTOUBillSortsMidnightSpanningBoundaries(t *testing.T) {
	// P0 regression: the day walk emits both window edges per day without
	// ordering, and a midnight-spanning window interleaved them so the bill
	// was computed against non-monotonic slices.
	offPeak := int64(60)
	windowStart := int16(23)
	windowEnd := int16(7)
	cfg := TariffConfig{PeakPrice: 120, OffPeak: &offPeak, WindowStart: &windowStart, WindowEnd: &windowEnd, ServicePrice: 50}

	// An eight-hour charge from 22:00 to 06:00 crosses 23:00 and midnight:
	// one peak hour (22-23) then seven off-peak hours (23-06); the 07:00
	// window edge lies beyond the end.
	start := time.Date(2026, 9, 15, 22, 0, 0, 0, time.UTC)
	end := time.Date(2026, 9, 16, 6, 0, 0, 0, time.UTC)
	bill := ComputeTOUBill(8000, start, end, cfg)

	if len(bill.Segments) != 2 {
		t.Fatalf("segments = %d, want 2", len(bill.Segments))
	}
	for index := 1; index < len(bill.Segments); index++ {
		if !bill.Segments[index-1].To.Equal(bill.Segments[index].From) {
			t.Fatalf("segment %d ends at %v but %d starts at %v",
				index-1, bill.Segments[index-1].To, index, bill.Segments[index].From)
		}
	}
	// Monotonic, non-overlapping slices ordered by time.
	if !bill.Segments[0].From.Equal(start) || !bill.Segments[1].To.Equal(end) {
		t.Fatalf("segment bounds = %v..%v, %v..%v", bill.Segments[0].From, bill.Segments[0].To, bill.Segments[1].From, bill.Segments[1].To)
	}
	if bill.Segments[0].Price != 120 || bill.Segments[1].Price != 60 {
		t.Fatalf("segment prices = %d/%d, want 120/60",
			bill.Segments[0].Price, bill.Segments[1].Price)
	}

	// The segment energies sum exactly to the metered total and each fee is
	// billed at its own price (1h peak, 7h off-peak... proportional split of
	// 8h: 1h peak (22-23), 7h off-peak (23-06)... the 07:00 edge is past the
	// end, so the last segment is off-peak only.
	energySum := int64(0)
	for _, segment := range bill.Segments {
		energySum += segment.EnergyWh
	}
	if energySum != 8000 {
		t.Fatalf("segment energies sum to %d, want 8000", energySum)
	}
	// 1h peak = 1000Wh at 120 = 120 cents; 7h off-peak = 7000Wh at 60 = 420 cents.
	if bill.ElectricityFeeCen != 120+420 {
		t.Fatalf("electricity fee = %d, want 540", bill.ElectricityFeeCen)
	}
	if bill.AmountCent != bill.ElectricityFeeCen+bill.ServiceFeeCent {
		t.Fatalf("amount %d != components %d+%d", bill.AmountCent, bill.ElectricityFeeCen, bill.ServiceFeeCent)
	}
}
