package main

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"
)

// The device's reporting half of the mock gateway (BE-I-02).
//
// The platform deliberately separates "the device accepted the command" from "the device reports
// what happened": only a receipt from the charger gateway moves an order from STARTING to CHARGING
// or from STOPPING to COMPLETED and writes the bill. A command simulator that never sends one
// therefore leaves every order parked in STARTING, which is exactly what the integration run saw -
// the commands all completed and no order ever advanced.
//
// This file gives the mock that second half without letting it pretend to be more than a
// development stand-in: it reports only the two facts of the frozen receipt contract, only for a
// command that actually COMPLETED, and only with the very service token the endpoint checks.

const (
	// chargerEventPath mirrors order.ChargerEventPath. It is repeated rather than imported because
	// the mock speaks the contract over HTTP like any other gateway, and importing the domain
	// package would suggest it shares more than a wire format.
	chargerEventPath = "/api/v1/internal/charger-events"

	// defaultAPIURL is where the platform's API listens by default, so a developer running the mock
	// beside it does not have to be told the address twice.
	defaultAPIURL = "http://127.0.0.1:8080"

	// defaultEnergyWh is what a simulated stop reports when nothing else is configured: 1 kWh, the
	// figure the closed-loop verification bills, so a manual run and the gate agree.
	defaultEnergyWh = 1000

	// receiptTimeout bounds one report. The receipt endpoint writes the order, the bill and the
	// outbox event in one transaction, so it is fast; a report slower than this is not going to
	// succeed by waiting.
	receiptTimeout = 5 * time.Second

	// The two receipt types of the frozen contract.
	eventTypeChargeStarted = "CHARGE_STARTED"
	eventTypeChargeStopped = "CHARGE_STOPPED"

	// The platform's own limits on a receipt id (order.minReceiptIDLength /
	// maxReceiptIDLength). They are enforced here so a misconfigured command id fails in the mock
	// with a clear message instead of being refused by the platform as an invalid receipt.
	minReceiptIDLength = 8
	maxReceiptIDLength = 128

	// receiptIDPrefix keeps a simulated fact recognisable in the platform's idempotency records: an
	// operator reading them can tell a report produced by the mock from one produced by a real
	// gateway.
	receiptIDPrefix = "mock-receipt-"
)

// receiptReporterConfig carries the reporter's dependencies, so a test can supply a logger and a
// client that talks to a stub platform instead of the real one.
type receiptReporterConfig struct {
	httpClient *http.Client
	logger     *slog.Logger
}

// receiptReporter posts the device facts the platform believes.
type receiptReporter struct {
	baseURL  string
	token    string
	energyWh int64
	client   *http.Client
	logger   *slog.Logger
}

// newReceiptReporter validates the reporting configuration.
//
// The token is required rather than defaulted: without it every report is refused with 401, and a
// mock that silently reports nothing looks exactly like the defect this code exists to fix.
func newReceiptReporter(cfg receiptReporterConfig, rawBaseURL, token string, energyWh int64) (*receiptReporter, error) {
	base := strings.TrimSpace(rawBaseURL)
	if base == "" {
		return nil, fmt.Errorf("the platform API base URL is required: there is nowhere to report a device fact")
	}
	parsed, err := url.Parse(base)
	if err != nil {
		return nil, fmt.Errorf("invalid platform API base URL %q: %w", base, err)
	}
	if parsed.Scheme != "http" && parsed.Scheme != "https" {
		return nil, fmt.Errorf("platform API base URL must be http or https, got %q", base)
	}
	if parsed.Host == "" {
		return nil, fmt.Errorf("platform API base URL must include a host, got %q", base)
	}
	if strings.TrimSpace(token) == "" {
		return nil, fmt.Errorf("the charger gateway service token is required: the receipt endpoint refuses an unauthenticated fact")
	}
	if energyWh < 0 {
		return nil, fmt.Errorf("metered energy must not be negative, got %d", energyWh)
	}
	client := cfg.httpClient
	if client == nil {
		client = &http.Client{Timeout: receiptTimeout}
	}
	logger := cfg.logger
	if logger == nil {
		logger = slog.New(slog.NewTextHandler(io.Discard, nil))
	}
	return &receiptReporter{
		baseURL:  strings.TrimSuffix(parsed.String(), "/"),
		token:    strings.TrimSpace(token),
		energyWh: energyWh,
		client:   client,
		logger:   logger,
	}, nil
}

// deviceFact is one fact of the frozen receipt contract, as the gateway sends it.
//
// The field names are the wire contract, not a Go convention: the platform validates every one of
// them, because every one of them decides an order's state or its bill.
type deviceFact struct {
	EventID    string `json:"eventId"`
	EventType  string `json:"eventType"`
	OrderNo    string `json:"orderNo"`
	ChargerID  int64  `json:"chargerId"`
	OccurredAt string `json:"occurredAt"`
	// EnergyWh is present only for a stop: the metered energy is what the bill is computed from, and
	// a stop without it cannot complete an order.
	EnergyWh     *int64 `json:"energyWh,omitempty"`
	MeterStartWh *int64 `json:"meterStartWh,omitempty"`
	MeterEndWh   *int64 `json:"meterEndWh,omitempty"`
	TraceID      string `json:"traceId,omitempty"`
}

// reportReceipt posts the device fact a completed charge command produced.
//
// It is deliberately allowed to fail. The command did reach the device, so answering the dispatcher
// with an error would make it re-run a command that already ran; the failed report is remembered
// instead, and the next delivery of the same command id retries it with an identical receipt.
func (g *mockGateway) reportReceipt(record *commandRecord) {
	reporter := g.receipts
	if reporter == nil || record == nil {
		return
	}

	fact, ok := g.buildFact(record, reporter)
	if !ok {
		return
	}

	if err := reporter.post(context.Background(), fact); err != nil {
		g.mu.Lock()
		commandID := record.commandID
		g.mu.Unlock()
		reporter.logger.Error("device fact was not reported; the next delivery of this command retries it",
			"command_id", commandID, "event_type", fact.EventType, "order_no", fact.OrderNo, "error", err)
		return
	}

	g.mu.Lock()
	record.receiptSent = true
	g.mu.Unlock()
	reporter.logger.Info("device fact reported",
		"event_id", fact.EventID, "event_type", fact.EventType,
		"order_no", fact.OrderNo, "charger_id", fact.ChargerID, "occurred_at", fact.OccurredAt)
}

// buildFact decides the fact for one record, once, and returns ok=false when there is nothing to
// report.
//
// Nothing to report means: the command is not a charge command, it did not complete, it was already
// reported, or its charger id is not the numeric id the contract carries. That last case is refused
// rather than guessed - a receipt naming no charger could not be applied, and inventing one would
// attach a device fact to somebody else's charger.
func (g *mockGateway) buildFact(record *commandRecord, reporter *receiptReporter) (deviceFact, bool) {
	g.mu.Lock()
	defer g.mu.Unlock()

	if record.receiptSent || record.status != statusCompleted {
		return deviceFact{}, false
	}
	eventType, isChargeAction := chargeReceipt(record.action)
	if !isChargeAction {
		return deviceFact{}, false
	}
	chargerID, err := strconv.ParseInt(strings.TrimSpace(record.chargerID), 10, 64)
	if err != nil || chargerID < 1 {
		reporter.logger.Warn("cannot report a device fact for a charger id that is not a charger id",
			"command_id", record.commandID, "charger_id", record.chargerID)
		return deviceFact{}, false
	}
	if strings.TrimSpace(record.orderNo) == "" {
		// A charge command without an order is refused by the command endpoint, so this is a defect
		// rather than input; reporting it would attach a fact to no order at all.
		reporter.logger.Warn("cannot report a device fact for a charge command without an order",
			"command_id", record.commandID)
		return deviceFact{}, false
	}

	// The id and the fact time are fixed the first time and reused afterwards, because a receipt is
	// idempotent on the pair: the same bytes replay, and the same id with a different fact time is a
	// conflict. Deciding them again on a retry would turn a retry into a conflict.
	if record.receiptID == "" {
		record.receiptID = receiptID(record.action, record.commandID)
		record.receiptOccurredAt = time.Now().UTC().Format(time.RFC3339Nano)
	}

	fact := deviceFact{
		EventID:    record.receiptID,
		EventType:  eventType,
		OrderNo:    strings.TrimSpace(record.orderNo),
		ChargerID:  chargerID,
		OccurredAt: record.receiptOccurredAt,
		TraceID:    strings.TrimSpace(record.traceID),
	}
	if eventType == eventTypeChargeStopped {
		// The meter readings are consistent with the metered energy on purpose: the platform refuses
		// a stop whose readings do not add up to the energy it is asked to bill.
		energy := reporter.energyWh
		start := int64(0)
		fact.EnergyWh = &energy
		fact.MeterStartWh = &start
		fact.MeterEndWh = &energy
	}
	return fact, true
}

// chargeReceipt maps a charge action to the fact the device reports for it. A station-level RESTART
// has no receipt: it changes no order.
func chargeReceipt(action string) (string, bool) {
	switch action {
	case actionStartCharging:
		return eventTypeChargeStarted, true
	case actionStopCharging:
		return eventTypeChargeStopped, true
	default:
		return "", false
	}
}

// receiptID names the fact in the platform's idempotency namespace.
//
// The prefix both identifies the producer and guarantees the minimum length the platform requires,
// so even the shortest command id produces an acceptable receipt id.
func receiptID(action, commandID string) string {
	id := receiptIDPrefix + strings.ToLower(action) + "-" + commandID
	if len(id) < minReceiptIDLength {
		id += strings.Repeat("0", minReceiptIDLength-len(id))
	}
	if len(id) > maxReceiptIDLength {
		// Command ids are generated by the platform and are far shorter than this; the guard exists
		// so a hand-made one cannot send the endpoint an id it must refuse.
		id = id[:maxReceiptIDLength]
	}
	return id
}

// post sends one fact and reports what the platform answered.
//
// A non-200 is an error even when the platform applied nothing: the caller decides whether to retry,
// and it can only decide correctly if it is told that the fact was not accepted.
func (r *receiptReporter) post(ctx context.Context, fact deviceFact) error {
	body, err := json.Marshal(fact)
	if err != nil {
		return fmt.Errorf("encode the device fact: %w", err)
	}
	ctx, cancel := context.WithTimeout(ctx, receiptTimeout)
	defer cancel()

	request, err := http.NewRequestWithContext(ctx, http.MethodPost, r.baseURL+chargerEventPath, bytes.NewReader(body))
	if err != nil {
		return fmt.Errorf("build the receipt request: %w", err)
	}
	request.Header.Set("Content-Type", "application/json")
	request.Header.Set("Authorization", "Bearer "+r.token)

	response, err := r.client.Do(request)
	if err != nil {
		return fmt.Errorf("post the device fact: %w", err)
	}
	defer func() { _ = response.Body.Close() }()

	payload, _ := io.ReadAll(io.LimitReader(response.Body, 4*1024))
	if response.StatusCode != http.StatusOK {
		return fmt.Errorf("the platform answered %d: %s", response.StatusCode, strings.TrimSpace(string(payload)))
	}
	return nil
}
