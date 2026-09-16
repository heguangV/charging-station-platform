package main

import (
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

// With running-meter reporting on, the stop settles what the simulated meter counted. A device
// that shows 0.25 kWh while charging and then claims the full session energy in its stop fact is
// not a device, and the app would show the customer two different numbers for one charge.
func TestStopSettlesTheMeteredEnergyNotTheConfiguredOne(t *testing.T) {
	gateway, platform := meterTestGateway(t, 20*time.Millisecond, 60)
	server := httptest.NewServer(http.HandlerFunc(gateway.handleCommand))
	t.Cleanup(server.Close)

	postChargeCommandWithin(t, server, "42", "cmd_meter_stop3", actionStartCharging, "ORD-METER-3", 3*time.Second)

	// Wait until the meter has reported something above zero but well below the session energy.
	var lastProgress int64
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		for _, fact := range platform.facts(t) {
			if fact.EventType == eventTypeChargeProgress && fact.EnergyWh != nil && *fact.EnergyWh > 0 {
				lastProgress = *fact.EnergyWh
			}
		}
		if lastProgress > 0 && lastProgress < defaultEnergyWh {
			break
		}
		time.Sleep(20 * time.Millisecond)
	}
	if lastProgress <= 0 || lastProgress >= defaultEnergyWh {
		t.Fatalf("the meter never reported a partial reading (last = %d)", lastProgress)
	}
	gateway.mu.Lock()
	counted, ok := gateway.meterEnergyLocked("ORD-METER-3")
	gateway.mu.Unlock()
	if !ok || counted != lastProgress {
		t.Fatalf("meter record = %d, %v; want the last reported reading %d", counted, ok, lastProgress)
	}

	postChargeCommandWithin(t, server, "42", "cmd_meter_stop3_end", actionStopCharging, "ORD-METER-3", 3*time.Second)

	var stopped *deviceFact
	for _, fact := range platform.facts(t) {
		if fact.EventType == eventTypeChargeStopped {
			copied := fact
			stopped = &copied
		}
	}
	if stopped == nil {
		t.Fatal("the gateway never reported a stop fact")
	}
	if stopped.EnergyWh == nil || *stopped.EnergyWh != lastProgress {
		t.Fatalf("stop fact energy = %v, want what the meter counted (%d)", stopped.EnergyWh, lastProgress)
	}
	if stopped.MeterEndWh == nil || *stopped.MeterEndWh != lastProgress || stopped.MeterStartWh == nil || *stopped.MeterStartWh != 0 {
		t.Fatalf("stop fact meter readings = %v/%v, want 0/%d", stopped.MeterStartWh, stopped.MeterEndWh, lastProgress)
	}
}
