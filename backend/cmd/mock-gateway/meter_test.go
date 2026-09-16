package main

import (
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

// meterTestGateway is a gateway whose charge reporting is live: a stub platform
// collects the facts, and the meter interval is short enough for a test to see
// several readings without waiting for production cadence.
func meterTestGateway(t *testing.T, interval time.Duration, chargeSeconds float64) (*mockGateway, *stubPlatform) {
	t.Helper()
	gateway, _ := newTestGateway(t)
	platform := &stubPlatform{}
	withReceipts(t, gateway, platform, defaultEnergyWh)
	gateway.meters = map[string]*chargingMeter{}
	gateway.meterInterval = interval
	gateway.chargeSeconds = chargeSeconds
	gateway.logger = slog.New(slog.NewTextHandler(io.Discard, nil))
	t.Cleanup(gateway.stopAllMeters)
	return gateway, platform
}

// The ramp is what a test can assert without a clock: nothing at the start, the
// configured energy once the session has taken its nominal time, and never more.
func TestRunningMeterRampsToTheSessionEnergy(t *testing.T) {
	gateway, _ := meterTestGateway(t, time.Hour, 60)
	meter := &chargingMeter{orderNo: "ORD-1", chargerID: 1, startedAt: time.Now().UTC()}
	gateway.receipts.energyWh = 1200

	if got := gateway.meterReadingFor(meter); got != 0 {
		t.Fatalf("reading at t=0 = %d, want 0", got)
	}
	meter.startedAt = time.Now().UTC().Add(-30 * time.Second)
	if got := gateway.meterReadingFor(meter); got < 500 || got > 700 {
		t.Fatalf("reading at half the session = %d, want about 600", got)
	}
	meter.startedAt = time.Now().UTC().Add(-2 * time.Minute)
	if got := gateway.meterReadingFor(meter); got != 1200 {
		t.Fatalf("reading past the session = %d, want the configured 1200", got)
	}
}

// A completed start begins reporting: the platform only shows a live amount if
// the device keeps telling it what the meter says.
func TestCompletedStartBeginsMeterReporting(t *testing.T) {
	gateway, platform := meterTestGateway(t, 20*time.Millisecond, 60)
	server := httptest.NewServer(http.HandlerFunc(gateway.handleCommand))
	t.Cleanup(server.Close)

	postChargeCommandWithin(t, server, "42", "cmd_meter_start", actionStartCharging, "ORD-METER-1", 3*time.Second)

	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		progress := 0
		for _, fact := range platform.facts(t) {
			if fact.EventType == eventTypeChargeProgress {
				progress++
				if fact.OrderNo != "ORD-METER-1" || fact.ChargerID != 42 || fact.EnergyWh == nil {
					t.Fatalf("progress fact = %#v", fact)
				}
			}
		}
		if progress >= 2 {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatal("the gateway never reported a running meter for a charge it started")
}

// The stop command ends the charge, so the meter must stop reporting: a reading
// for a completed order is refused by the platform and would only be noise.
func TestCompletedStopEndsMeterReporting(t *testing.T) {
	gateway, platform := meterTestGateway(t, 20*time.Millisecond, 60)
	server := httptest.NewServer(http.HandlerFunc(gateway.handleCommand))
	t.Cleanup(server.Close)

	postChargeCommandWithin(t, server, "42", "cmd_meter_start2", actionStartCharging, "ORD-METER-2", 3*time.Second)
	postChargeCommandWithin(t, server, "42", "cmd_meter_stop2", actionStopCharging, "ORD-METER-2", 3*time.Second)

	// Let any in-flight tick land, then count over a window longer than the interval.
	time.Sleep(100 * time.Millisecond)
	before := 0
	for _, fact := range platform.facts(t) {
		if fact.EventType == eventTypeChargeProgress {
			before++
		}
	}
	time.Sleep(200 * time.Millisecond)
	after := 0
	for _, fact := range platform.facts(t) {
		if fact.EventType == eventTypeChargeProgress {
			after++
		}
	}
	if after > before {
		t.Fatalf("the meter kept reporting after the stop: %d readings, then %d", before, after)
	}
	if len(gateway.meters) != 0 {
		t.Fatalf("meters still running after the stop: %d", len(gateway.meters))
	}
}

// A meter that reaches its configured energy stops its own ticker but remains available so the
// stop fact can settle exactly what it showed. Stopping that order later must not close the same
// channel twice and crash the gateway.
func TestCompletedStopAfterMeterRampDoesNotPanic(t *testing.T) {
	gateway, _ := meterTestGateway(t, 10*time.Millisecond, 0.01)
	server := httptest.NewServer(http.HandlerFunc(gateway.handleCommand))
	t.Cleanup(server.Close)

	postChargeCommandWithin(t, server, "42", "cmd_meter_ramp_start", actionStartCharging, "ORD-METER-RAMP", 3*time.Second)
	time.Sleep(50 * time.Millisecond)
	postChargeCommandWithin(t, server, "42", "cmd_meter_ramp_stop", actionStopCharging, "ORD-METER-RAMP", 3*time.Second)

	if len(gateway.meters) != 0 {
		t.Fatalf("meters still retained after the completed stop: %d", len(gateway.meters))
	}
}
