package main

import (
	"context"
	"fmt"
	"math"
	"strconv"
	"strings"
	"sync"
	"time"
)

// The running meter: what a real charger reports while it is charging (OCPP MeterValues in the
// field). Without it the platform learns the energy only from the stop receipt, so an app can show
// nothing but a guess during a charge.
//
// The simulated meter ramps linearly from 0 to the configured session energy over
// NCS_MOCK_GATEWAY_CHARGE_SECONDS and then holds that value. A stop still reports the full
// configured energy, so a session shorter than the ramp converges upwards at the end: the mock
// answers commands at once, it does not pretend to know how long a session really lasted.

// chargingMeter tracks one charging order's simulated meter.
type chargingMeter struct {
	orderNo   string
	chargerID int64
	traceID   string
	startedAt time.Time
	stop      chan struct{}
	stopOnce  sync.Once
	// lastEnergy is what the simulated meter has counted so far. It survives the moment reporting
	// stops at the end of the ramp, because the stop fact has to settle the energy the device
	// actually delivered: a device that reports 0.25 kWh while charging and then claims 1 kWh in its
	// stop fact is not a device.
	lastEnergy int64
}

func (m *chargingMeter) stopReporting() {
	m.stopOnce.Do(func() { close(m.stop) })
}

// startMeterReporting begins reporting the running meter for one order.
func (g *mockGateway) startMeterReporting(orderNo, chargerID, traceID string) {
	if g.receipts == nil || g.meterInterval <= 0 || g.chargeSeconds <= 0 {
		return
	}
	orderNo = strings.TrimSpace(orderNo)
	if orderNo == "" {
		return
	}
	id, err := strconv.ParseInt(strings.TrimSpace(chargerID), 10, 64)
	if err != nil || id < 1 {
		return
	}

	g.mu.Lock()
	if _, exists := g.meters[orderNo]; exists {
		g.mu.Unlock()
		return
	}
	meter := &chargingMeter{
		orderNo:   orderNo,
		chargerID: id,
		traceID:   strings.TrimSpace(traceID),
		startedAt: time.Now().UTC(),
		stop:      make(chan struct{}),
	}
	g.meters[orderNo] = meter
	g.mu.Unlock()

	go g.runMeter(meter)
	g.logger.Info("simulated meter started reporting",
		"order_no", orderNo, "charger_id", id, "interval", g.meterInterval.String(),
		"session_seconds", g.chargeSeconds)
}

// finishMeterReporting stops the ticker once the ramp is complete, keeping the record: the stop
// fact still has to report what the meter counted.
func (g *mockGateway) finishMeterReporting(orderNo string) {
	orderNo = strings.TrimSpace(orderNo)
	g.mu.Lock()
	meter, exists := g.meters[orderNo]
	g.mu.Unlock()
	if !exists {
		return
	}
	meter.stopReporting()
}

// meterEnergyLocked reports what the simulated meter counted for an order, if it ever reported.
//
// The caller must hold g.mu: buildFact already runs under it, and locking again inside would
// deadlock the stop receipt - a hang, which is the worst failure mode for a receipt path.
func (g *mockGateway) meterEnergyLocked(orderNo string) (int64, bool) {
	meter, exists := g.meters[strings.TrimSpace(orderNo)]
	if !exists {
		return 0, false
	}
	return meter.lastEnergy, true
}

// stopMeterReporting stops the meter of one order (the charge ended).
func (g *mockGateway) stopMeterReporting(orderNo string) {
	orderNo = strings.TrimSpace(orderNo)
	g.mu.Lock()
	meter, exists := g.meters[orderNo]
	if exists {
		delete(g.meters, orderNo)
	}
	g.mu.Unlock()
	if !exists {
		return
	}
	meter.stopReporting()
	g.logger.Info("simulated meter stopped reporting", "order_no", orderNo)
}

// stopAllMeters is the shutdown path: no goroutine keeps reporting after the process is asked to go.
func (g *mockGateway) stopAllMeters() {
	g.mu.Lock()
	meters := make([]*chargingMeter, 0, len(g.meters))
	for orderNo, meter := range g.meters {
		meters = append(meters, meter)
		delete(g.meters, orderNo)
	}
	g.mu.Unlock()
	for _, meter := range meters {
		meter.stopReporting()
	}
}

func (g *mockGateway) runMeter(meter *chargingMeter) {
	ticker := time.NewTicker(g.meterInterval)
	defer ticker.Stop()
	sequence := 0
	for {
		select {
		case <-meter.stop:
			return
		case <-ticker.C:
			sequence++
			if err := g.reportMeterReading(meter, sequence); err != nil {
				// A failed report is not fatal: the next tick carries a higher reading, and a
				// reading is absolute, so a lost one changes nothing but the update frequency.
				g.logger.Warn("running meter reading was not reported",
					"order_no", meter.orderNo, "charger_id", meter.chargerID, "error", err)
			}
			// Once the simulated session has delivered its energy the meter has nothing left to
			// add: repeating the same reading would only be ignored by the platform (a reading is
			// monotonic), so the reporting stops until a stop command ends the charge.
			if g.meterReadingFor(meter) >= g.receipts.energyWh {
				g.finishMeterReporting(meter.orderNo)
				g.logger.Info("simulated meter reached the session energy and stopped reporting",
					"order_no", meter.orderNo, "energy_wh", g.receipts.energyWh)
				return
			}
		}
	}
}

// meterReadingFor is the energy the simulated meter shows after elapsed time.
func (g *mockGateway) meterReadingFor(meter *chargingMeter) int64 {
	total := g.receipts.energyWh
	elapsed := time.Since(meter.startedAt).Seconds()
	ratio := elapsed / g.chargeSeconds
	if ratio > 1 {
		ratio = 1
	}
	if ratio < 0 {
		ratio = 0
	}
	return int64(math.Round(float64(total) * ratio))
}

func (g *mockGateway) reportMeterReading(meter *chargingMeter, sequence int) error {
	energy := g.meterReadingFor(meter)
	g.mu.Lock()
	meter.lastEnergy = energy
	g.mu.Unlock()
	fact := deviceFact{
		EventID:    fmt.Sprintf("mock-meter-%s-%d", meter.orderNo, sequence),
		EventType:  eventTypeChargeProgress,
		OrderNo:    meter.orderNo,
		ChargerID:  meter.chargerID,
		OccurredAt: time.Now().UTC().Format(time.RFC3339Nano),
		TraceID:    meter.traceID,
		EnergyWh:   &energy,
	}
	if err := g.receipts.post(context.Background(), fact); err != nil {
		return err
	}
	g.logger.Info("running meter reported",
		"event_id", fact.EventID, "order_no", meter.orderNo,
		"charger_id", meter.chargerID, "energy_wh", energy)
	return nil
}
