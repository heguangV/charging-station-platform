package main

import (
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"
)

// The mock gateway is the only device the closed loop can talk to, and answering a command is not
// enough to close that loop: the platform waits for the device to report what physically happened.
// These tests cover the reporting half - which facts it sends, which it must not, and what happens
// when the platform refuses one.

const testReceiptToken = "mock-gateway-test-token"

// stubPlatform stands in for the receipt endpoint. It records every fact it is sent and answers
// with a status a test can change, so both the accepted and the refused path are exercised without
// a database.
type stubPlatform struct {
	mu      sync.Mutex
	bodies  []string
	headers []http.Header
	status  int
	failN   int
}

func (p *stubPlatform) handler() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		p.mu.Lock()
		p.bodies = append(p.bodies, string(body))
		p.headers = append(p.headers, r.Header.Clone())
		status := p.status
		if p.failN > 0 {
			p.failN--
			status = http.StatusInternalServerError
		}
		p.mu.Unlock()

		if status == 0 {
			status = http.StatusOK
		}
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(status)
		_, _ = w.Write([]byte(`{"success":true,"code":0,"message":"ok","data":{}}`))
	}
}

// facts decodes what the stub was sent.
func (p *stubPlatform) facts(t *testing.T) []deviceFact {
	t.Helper()
	p.mu.Lock()
	defer p.mu.Unlock()
	decoded := make([]deviceFact, 0, len(p.bodies))
	for _, body := range p.bodies {
		fact := deviceFact{}
		if err := json.Unmarshal([]byte(body), &fact); err != nil {
			t.Fatalf("the gateway posted a body that is not a device fact: %s (%v)", body, err)
		}
		decoded = append(decoded, fact)
	}
	return decoded
}

func (p *stubPlatform) count() int {
	p.mu.Lock()
	defer p.mu.Unlock()
	return len(p.bodies)
}

// withReceipts turns reporting on for a test gateway and points it at a stub platform.
func withReceipts(t *testing.T, gateway *mockGateway, platform *stubPlatform, energyWh int64) *httptest.Server {
	t.Helper()
	server := httptest.NewServer(platform.handler())
	t.Cleanup(server.Close)

	reporter, err := newReceiptReporter(receiptReporterConfig{
		logger: slog.New(slog.NewTextHandler(io.Discard, nil)),
	}, server.URL, testReceiptToken, energyWh)
	if err != nil {
		t.Fatalf("newReceiptReporter() error = %v", err)
	}
	gateway.receipts = reporter
	return server
}

// A start command the device completed is a device fact, and the platform only learns the charge
// really began from this receipt. Without it the order stays in STARTING forever, which is the
// defect this reporting half exists to close.
func TestMockGatewayReportsAStartedFact(t *testing.T) {
	gateway, server := newTestGateway(t)
	platform := &stubPlatform{}
	withReceipts(t, gateway, platform, defaultEnergyWh)

	status, body := postChargeCommandWithin(t, server, "28", "cmd_report_start", actionStartCharging, "ORD-2001", 3*time.Second)
	if status != http.StatusOK {
		t.Fatalf("status = %d (%s), want 200", status, body)
	}

	facts := platform.facts(t)
	if len(facts) != 1 {
		t.Fatalf("the gateway reported %d fact(s), want exactly 1", len(facts))
	}
	fact := facts[0]
	if fact.EventType != eventTypeChargeStarted {
		t.Fatalf("eventType = %q, want %q", fact.EventType, eventTypeChargeStarted)
	}
	if fact.OrderNo != "ORD-2001" {
		t.Fatalf("orderNo = %q, want the command's order", fact.OrderNo)
	}
	if fact.ChargerID != 28 {
		t.Fatalf("chargerId = %d, want 28 as a number", fact.ChargerID)
	}
	if fact.OccurredAt == "" {
		t.Fatal("a device fact must carry the time the device reports")
	}
	if _, err := time.Parse(time.RFC3339Nano, fact.OccurredAt); err != nil {
		t.Fatalf("occurredAt = %q is not a device fact time: %v", fact.OccurredAt, err)
	}
	if fact.EnergyWh != nil || fact.MeterStartWh != nil || fact.MeterEndWh != nil {
		t.Fatalf("a start receipt carries no metered energy, got %+v", fact)
	}
	if _, err := time.Parse(time.RFC3339Nano, fact.OccurredAt); err == nil && !strings.HasSuffix(fact.OccurredAt, "Z") {
		t.Fatalf("occurredAt = %q must be UTC", fact.OccurredAt)
	}
}

// A stop receipt is what completes the order and writes the bill, and the bill is computed from the
// energy it reports: the reading has to be there, and it has to add up.
func TestMockGatewayReportsAStoppedFactWithConsistentMeters(t *testing.T) {
	gateway, server := newTestGateway(t)
	platform := &stubPlatform{}
	withReceipts(t, gateway, platform, 2500)

	status, body := postChargeCommandWithin(t, server, "28", "cmd_report_stop", actionStopCharging, "ORD-2002", 3*time.Second)
	if status != http.StatusOK {
		t.Fatalf("status = %d (%s), want 200", status, body)
	}

	facts := platform.facts(t)
	if len(facts) != 1 {
		t.Fatalf("the gateway reported %d fact(s), want exactly 1", len(facts))
	}
	fact := facts[0]
	if fact.EventType != eventTypeChargeStopped {
		t.Fatalf("eventType = %q, want %q", fact.EventType, eventTypeChargeStopped)
	}
	if fact.EnergyWh == nil || *fact.EnergyWh != 2500 {
		t.Fatalf("energyWh = %v, want the configured 2500", fact.EnergyWh)
	}
	if fact.MeterStartWh == nil || fact.MeterEndWh == nil {
		t.Fatalf("a stop receipt reports its meter readings, got %+v", fact)
	}
	// The platform refuses a stop whose readings do not add up to the energy it is asked to bill.
	if *fact.MeterEndWh-*fact.MeterStartWh != *fact.EnergyWh {
		t.Fatalf("meter readings %d..%d do not add up to energyWh %d",
			*fact.MeterStartWh, *fact.MeterEndWh, *fact.EnergyWh)
	}
}

// Reporting is off unless it is asked for. The closed-loop gate asserts that a device merely
// accepting a command does not move an order, so the default must stay a command simulator.
func TestMockGatewayReportsNothingUnlessItIsAskedTo(t *testing.T) {
	gateway, server := newTestGateway(t)
	platform := &stubPlatform{}
	withReceipts(t, gateway, platform, defaultEnergyWh)
	// The helper installs a reporter, so this test turns it back off to model the default.
	gateway.receipts = nil

	status, body := postChargeCommandWithin(t, server, "28", "cmd_no_report", actionStartCharging, "ORD-2003", 3*time.Second)
	if status != http.StatusOK {
		t.Fatalf("status = %d (%s), want 200", status, body)
	}
	if platform.count() != 0 {
		t.Fatalf("the default configuration reported %d fact(s), want none", platform.count())
	}
}

// A device that refused to start did not start. Reporting a start fact for it would move the order
// to CHARGING on a device that never charged anything.
func TestMockGatewayReportsNothingForACommandTheDeviceFailed(t *testing.T) {
	gateway, server := newTestGateway(t)
	gateway.failing["29"] = true
	platform := &stubPlatform{}
	withReceipts(t, gateway, platform, defaultEnergyWh)

	status, body := postChargeCommandWithin(t, server, "29", "cmd_failed_report", actionStartCharging, "ORD-2009", 3*time.Second)
	if status != http.StatusOK {
		t.Fatalf("status = %d (%s), want 200", status, body)
	}
	if !strings.Contains(body, `"status":"FAILED"`) {
		t.Fatalf("the device refused the command, so the answer must say FAILED: %s", body)
	}
	if platform.count() != 0 {
		t.Fatalf("a failed charge command reported %d fact(s), want none", platform.count())
	}
}

// A station-level restart belongs to no order, so there is no fact to report for it.
func TestMockGatewayReportsNothingForARestart(t *testing.T) {
	gateway, server := newTestGateway(t)
	platform := &stubPlatform{}
	withReceipts(t, gateway, platform, defaultEnergyWh)

	if status, _ := postCommand(t, server, "28", "cmd_restart_report", "RESTART"); status != http.StatusOK {
		t.Fatalf("status = %d, want 200", status)
	}
	if platform.count() != 0 {
		t.Fatalf("a restart reported %d fact(s), want none", platform.count())
	}
}

// The dispatcher retries, and a retry must not tell the platform the device started twice: a second
// start fact is a second bill waiting to happen.
func TestMockGatewayReportsOneFactHoweverManyDeliveriesArrive(t *testing.T) {
	gateway, server := newTestGateway(t)
	platform := &stubPlatform{}
	withReceipts(t, gateway, platform, defaultEnergyWh)

	for attempt := 1; attempt <= 3; attempt++ {
		status, body := postChargeCommandWithin(t, server, "28", "cmd_one_fact", actionStartCharging, "ORD-2004", 3*time.Second)
		if status != http.StatusOK {
			t.Fatalf("delivery %d status = %d (%s), want 200", attempt, status, body)
		}
	}

	if platform.count() != 1 {
		t.Fatalf("%d deliveries produced %d fact(s), want exactly 1", 3, platform.count())
	}
	if executions := gateway.executionsCount(); executions != 1 {
		t.Fatalf("device work executed %d time(s), want 1", executions)
	}
}

// A report the platform refused is not lost. The device did the work, so the command is still
// answered as completed; the fact is remembered and re-sent on the next delivery, and it is re-sent
// byte-for-byte, because the receipt id is idempotent on its payload.
func TestMockGatewayRetriesAFactThePlatformRefused(t *testing.T) {
	gateway, server := newTestGateway(t)
	platform := &stubPlatform{}
	withReceipts(t, gateway, platform, defaultEnergyWh)
	platform.mu.Lock()
	platform.failN = 1
	platform.mu.Unlock()

	status, body := postChargeCommandWithin(t, server, "28", "cmd_retry_report", actionStartCharging, "ORD-2005", 3*time.Second)
	if status != http.StatusOK {
		t.Fatalf("a refused report must not fail the command: status = %d (%s)", status, body)
	}
	if platform.count() != 1 {
		t.Fatalf("the first delivery posted %d fact(s), want 1", platform.count())
	}

	// The retry of the same command sends the same fact again.
	status, body = postChargeCommandWithin(t, server, "28", "cmd_retry_report", actionStartCharging, "ORD-2005", 3*time.Second)
	if status != http.StatusOK {
		t.Fatalf("retry status = %d (%s), want 200", status, body)
	}
	if platform.count() != 2 {
		t.Fatalf("the retry posted %d fact(s) in total, want 2", platform.count())
	}
	platform.mu.Lock()
	first, second := platform.bodies[0], platform.bodies[1]
	platform.mu.Unlock()
	if first != second {
		t.Fatalf("the retry changed the fact:\n first: %s\nsecond: %s", first, second)
	}
	// The device is not asked to do the work again, only the platform is told again.
	if executions := gateway.executionsCount(); executions != 1 {
		t.Fatalf("the retry executed device work %d time(s), want 1", executions)
	}
}

// Once the platform accepted the fact, a later delivery must not send it again: the receipt is the
// platform's idempotency key, and reusing a key is only ever a replay or a conflict.
func TestMockGatewayStopsReportingAfterThePlatformAcceptedTheFact(t *testing.T) {
	gateway, server := newTestGateway(t)
	platform := &stubPlatform{}
	withReceipts(t, gateway, platform, defaultEnergyWh)

	if status, _ := postChargeCommandWithin(t, server, "28", "cmd_settled_report", actionStartCharging, "ORD-2006", 3*time.Second); status != http.StatusOK {
		t.Fatalf("status = %d, want 200", status)
	}
	if status, _ := postChargeCommandWithin(t, server, "28", "cmd_settled_report", actionStartCharging, "ORD-2006", 3*time.Second); status != http.StatusOK {
		t.Fatalf("retry status = %d, want 200", status)
	}
	if platform.count() != 1 {
		t.Fatalf("an accepted fact was posted %d time(s), want 1", platform.count())
	}
}

// The receipt endpoint carries a numeric charger id. A path that is not one cannot be turned into a
// fact, and guessing an id would attach the device fact to another charger.
func TestMockGatewayReportsNothingForANonNumericCharger(t *testing.T) {
	gateway, server := newTestGateway(t)
	platform := &stubPlatform{}
	withReceipts(t, gateway, platform, defaultEnergyWh)

	status, body := postChargeCommandWithin(t, server, "C01", "cmd_bad_charger", actionStartCharging, "ORD-2007", 3*time.Second)
	if status != http.StatusOK {
		t.Fatalf("status = %d (%s), want 200: the command itself is still answered", status, body)
	}
	if platform.count() != 0 {
		t.Fatalf("a non-numeric charger produced %d fact(s), want none", platform.count())
	}
}

// The platform refuses a receipt id outside its own bounds, so the mock must not be able to produce
// one: a report refused for its id would be retried forever.
func TestReceiptIDStaysInsideThePlatformContract(t *testing.T) {
	cases := map[string]string{
		"short command id":  "1",
		"normal command id": "cmd_01HQ2Z8Q4K9VJ7X3M5N7P9R1T3",
		"long command id":   strings.Repeat("c", 200),
	}
	for name, commandID := range cases {
		t.Run(name, func(t *testing.T) {
			for _, action := range []string{actionStartCharging, actionStopCharging} {
				id := receiptID(action, commandID)
				if len(id) < minReceiptIDLength || len(id) > maxReceiptIDLength {
					t.Fatalf("receipt id %q has length %d, outside %d..%d",
						id, len(id), minReceiptIDLength, maxReceiptIDLength)
				}
				if !strings.HasPrefix(id, receiptIDPrefix) {
					t.Fatalf("receipt id %q must be recognisable as a simulated fact", id)
				}
			}
		})
	}
	// The same command always names the same fact, which is what makes a retry a replay.
	if receiptID(actionStartCharging, "cmd_same") != receiptID(actionStartCharging, "cmd_same") {
		t.Fatal("the receipt id must be decided by the command, not by the moment it is built")
	}
	if receiptID(actionStartCharging, "cmd_same") == receiptID(actionStopCharging, "cmd_same") {
		t.Fatal("a start and a stop of one command id are different facts and need different receipt ids")
	}
}

// A reporter that cannot report is worse than none: it would look like the defect rather than
// reporting it. Every unusable configuration is refused up front.
func TestNewReceiptReporterRejectsUnusableConfiguration(t *testing.T) {
	cases := map[string]struct {
		baseURL  string
		token    string
		energyWh int64
	}{
		"no api url":      {baseURL: "", token: testReceiptToken},
		"blank api url":   {baseURL: "   ", token: testReceiptToken},
		"no scheme":       {baseURL: "127.0.0.1:8080", token: testReceiptToken},
		"wrong scheme":    {baseURL: "ftp://127.0.0.1:8080", token: testReceiptToken},
		"no host":         {baseURL: "http://", token: testReceiptToken},
		"no token":        {baseURL: "http://127.0.0.1:8080", token: ""},
		"negative energy": {baseURL: "http://127.0.0.1:8080", token: testReceiptToken, energyWh: -1},
	}
	for name, cfg := range cases {
		t.Run(name, func(t *testing.T) {
			if _, err := newReceiptReporter(receiptReporterConfig{}, cfg.baseURL, cfg.token, cfg.energyWh); err == nil {
				t.Fatal("expected the configuration to be refused")
			}
		})
	}
}

// The report is a service call, not a session call: it carries the gateway token and nothing else,
// because the endpoint advances an order and a forged fact would start or end a bill.
func TestReportCarriesTheServiceToken(t *testing.T) {
	gateway, server := newTestGateway(t)
	platform := &stubPlatform{}
	withReceipts(t, gateway, platform, defaultEnergyWh)

	if status, _ := postChargeCommandWithin(t, server, "28", "cmd_auth_report", actionStartCharging, "ORD-2008", 3*time.Second); status != http.StatusOK {
		t.Fatalf("status = %d, want 200", status)
	}
	platform.mu.Lock()
	defer platform.mu.Unlock()
	if len(platform.headers) != 1 {
		t.Fatalf("expected one report, got %d", len(platform.headers))
	}
	if got := platform.headers[0].Get("Authorization"); got != "Bearer "+testReceiptToken {
		t.Fatalf("Authorization = %q, want the gateway service token", got)
	}
	if got := platform.headers[0].Get("Content-Type"); got != "application/json" {
		t.Fatalf("Content-Type = %q, want application/json", got)
	}
}
