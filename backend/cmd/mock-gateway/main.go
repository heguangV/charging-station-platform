// Command mock-gateway is a development-only HTTP stand-in for the charger gateway.
//
// It exists so the command half of the closed loop can be exercised end to end before the real
// gateway protocol exists: the worker dispatches a command to it, it answers, and the platform
// records the device outcome. It speaks the same HTTP contract as the dispatcher
// (backend/cmd/worker/adapters.go) and nothing else.
//
// It is NOT a charger gateway. It does not implement Modbus, OCPP or any device protocol, and it
// must never be enabled in a production configuration: the dispatcher only reaches it when the
// gateway address is explicitly pointed at it, and the worker's default configuration has no
// gateway address at all, so a deployment that forgot to set one fails instead of silently
// dispatching to a mock.
//
// Answering a command is only half of what a gateway does. A charge command that the device merely
// accepted advances no order - the platform deliberately waits for the device to report what
// physically happened - so a mock that never sends a receipt leaves every order parked in STARTING
// and no charge flow can be exercised through the UI. With -receipts the mock also plays the
// device's reporting half: after a charge command completes it posts the matching CHARGE_STARTED or
// CHARGE_STOPPED fact to backend/internal/order's receipt endpoint (see receipts.go).
//
// That reporting is off by default. verify-closed-loop.sh asserts that an accepted command does not
// move an order, and it drives the receipts itself so it can place the device fact times inside a
// known tariff window; a mock that reported on its own would silently change what that gate proves.
package main

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/heguangV/charging-station-platform/backend/internal/httpapi"
)

// defaultAddr keeps the mock off the ports the platform itself uses.
const defaultAddr = "127.0.0.1:8091"

// Device statuses the mock answers with. Only a COMPLETED charge command is a physical fact worth
// reporting: a device that refused to start did not start, so there is no receipt to send.
const statusCompleted = "COMPLETED"

// The two charge actions of the frozen command contract that name an order.
const (
	actionStartCharging = "START_CHARGING"
	actionStopCharging  = "STOP_CHARGING"
)

// mockHeader marks every response so nobody mistakes this process for a device.
const mockHeader = "X-NCS-Mock-Gateway"

func main() {
	var (
		addrFlag    = flag.String("addr", envOr("NCS_MOCK_GATEWAY_ADDR", defaultAddr), "listen address")
		delayFlag   = flag.Duration("delay", durationOr(os.Getenv("NCS_MOCK_GATEWAY_DELAY"), 0), "simulated device latency before answering")
		resultFlag  = flag.String("result", envOr("NCS_MOCK_GATEWAY_RESULT", "COMPLETED"), "outcome to report: COMPLETED or FAILED")
		failingFlag = flag.String("failing-chargers", envOr("NCS_MOCK_GATEWAY_FAILING_CHARGERS", ""), "comma separated charger ids answered with FAILED")
		receiptFlag = flag.Bool("receipts", boolOr("NCS_MOCK_GATEWAY_RECEIPTS", false), "also report the device's CHARGE_STARTED/CHARGE_STOPPED facts to the platform after a charge command completes")
		apiFlag     = flag.String("api-url", envOr("NCS_MOCK_GATEWAY_API_URL", defaultAPIURL), "platform API base URL the device facts are reported to (only used with -receipts)")
		tokenFlag   = flag.String("gateway-token", envOr("NCS_CHARGER_GATEWAY_TOKEN", ""), "service token the receipt endpoint accepts; required with -receipts, never invented")
		energyFlag  = flag.Int64("energy-wh", int64Or("NCS_MOCK_GATEWAY_ENERGY_WH", defaultEnergyWh), "metered energy a simulated stop reports, in watt-hours")
		meterFlag   = flag.Duration("meter-interval", durationOr(os.Getenv("NCS_MOCK_GATEWAY_METER_INTERVAL"), defaultMeterInterval), "how often a charging device reports its running meter (0 disables progress receipts)")
		chargeFlag  = flag.Float64("charge-seconds", floatOr(os.Getenv("NCS_MOCK_GATEWAY_CHARGE_SECONDS"), defaultChargeSeconds), "simulated seconds a session takes to reach -energy-wh; the running meter ramps linearly over it")
	)
	flag.Parse()

	logger := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelInfo}))
	logger.Warn("starting the development mock charger gateway; it implements no device protocol and must not be used in production",
		"addr", *addrFlag, "result", *resultFlag, "delay", delayFlag.String(), "receipts", *receiptFlag)

	gateway := &mockGateway{
		logger:        logger,
		delay:         *delayFlag,
		result:        strings.ToUpper(strings.TrimSpace(*resultFlag)),
		failing:       parseIDSet(*failingFlag),
		commands:      map[string]*commandRecord{},
		meters:        map[string]*chargingMeter{},
		meterInterval: *meterFlag,
		chargeSeconds: *chargeFlag,
	}

	if *receiptFlag {
		// The token has no default for the same reason the endpoint has none: a receipt advances an
		// order and starts or ends a bill, so a gateway that guesses a credential is a gateway that
		// either fails every report or, worse, is pointed at a platform that accepts it.
		token := strings.TrimSpace(*tokenFlag)
		if token == "" {
			logger.Error("device fact reporting needs NCS_CHARGER_GATEWAY_TOKEN: the platform refuses every receipt without it")
			os.Exit(2)
		}
		reporter, err := newReceiptReporter(receiptReporterConfig{
			httpClient: &http.Client{Timeout: receiptTimeout},
			logger:     logger,
		}, *apiFlag, token, *energyFlag)
		if err != nil {
			logger.Error("invalid device fact reporting configuration", "error", err)
			os.Exit(2)
		}
		gateway.receipts = reporter
		logger.Info("the mock gateway will report device facts",
			"api_url", reporter.baseURL, "endpoint", chargerEventPath, "energy_wh", reporter.energyWh)
		if gateway.meterInterval > 0 {
			logger.Info("the mock gateway will also report a running meter while charging",
				"interval", gateway.meterInterval.String(), "session_seconds", gateway.chargeSeconds)
		}
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/healthz", gateway.handleHealth)
	mux.HandleFunc("/chargers/", gateway.handleCommand)

	server := &http.Server{
		Addr:              *addrFlag,
		Handler:           mux,
		ReadHeaderTimeout: 5 * time.Second,
	}

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()
	go func() {
		<-ctx.Done()
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = server.Shutdown(shutdownCtx)
	}()

	listener, err := net.Listen("tcp", *addrFlag)
	if err != nil {
		logger.Error("listen", "error", err)
		os.Exit(1)
	}
	logger.Info("mock gateway listening", "addr", listener.Addr().String())
	if err := server.Serve(listener); err != nil && !errors.Is(err, http.ErrServerClosed) {
		logger.Error("serve", "error", err)
		os.Exit(1)
	}
	logger.Info("mock gateway stopped", "commands", gateway.commandCount(), "executed", gateway.executedCount())
}

// mockGateway answers device commands.
type mockGateway struct {
	logger  *slog.Logger
	delay   time.Duration
	result  string
	failing map[string]bool

	// receipts is the device's reporting half, and it is nil unless the operator asked for it: the
	// mock is a command simulator by default, because the closed-loop gate asserts that a device
	// accepting a command does not by itself move an order.
	receipts *receiptReporter

	mu       sync.Mutex
	commands map[string]*commandRecord
	// meters are the charges whose simulated meter is still reporting. The keys are order
	// numbers, because that is what a progress receipt identifies.
	meters map[string]*chargingMeter
	// meterInterval is how often the running meter is reported, and chargeSeconds how long the
	// simulated session takes to reach the configured energy. Zero interval disables reporting.
	meterInterval time.Duration
	chargeSeconds float64

	// executions counts how many times device work was actually simulated, so a test can show that
	// concurrent duplicates did not execute it twice.
	executions int
}

// commandRecord is what the gateway remembers about one command id.
//
// It exists so the gateway is idempotent the way a real one must be: a retry of the SAME request
// is answered with the outcome that was already decided (the device is not restarted twice), while
// the same command id carrying a DIFFERENT request is a conflict rather than a silent overwrite -
// overwriting was the review finding, because it hides a caller that reuses command ids for
// different work.
type commandRecord struct {
	commandID string
	chargerID string
	// orderNo is part of the request's identity: the same command id carrying a
	// different order is a different piece of work, and answering it with the first
	// outcome would attach a device result to the wrong order.
	orderNo string
	action  string
	traceID string
	// done is closed when the outcome is decided. A duplicate that arrives while the first request
	// is still waiting for the device waits on it instead of executing the command again.
	done        chan struct{}
	inProgress  bool
	status      string
	detail      string
	attempts    int
	firstSeenAt time.Time

	// The device fact this command produces, decided once and then kept.
	//
	// A receipt is idempotent on its id AND on its payload: re-sending the same fact must replay the
	// first result, while reusing the id with a different fact time is a conflict. So the id and the
	// fact time are fixed the first time the fact is built and reused by every later attempt, which
	// is what lets a report that failed be retried instead of lost.
	receiptID         string
	receiptOccurredAt string
	receiptSent       bool
}

// outcome is the decided result, or ok=false while the device has not answered yet.
func (r *commandRecord) outcome() (status string, detail string, ok bool) {
	if r.inProgress {
		return "", "", false
	}
	return r.status, r.detail, true
}

type commandRequest struct {
	CommandID string `json:"command_id"`
	ChargerID string `json:"charger_id"`
	OrderNo   string `json:"order_no"`
	Action    string `json:"action"`
	TraceID   string `json:"trace_id"`
}

// supportedActions mirrors the frozen command contract: the station-level restart plus the two
// charge actions. The charge actions name an order; a restart does not.
var supportedActions = map[string]bool{"RESTART": true, "START_CHARGING": true, "STOP_CHARGING": true}

// actionNeedsOrder mirrors the same contract's field requirement.
func actionNeedsOrder(action string) bool {
	return action == actionStartCharging || action == actionStopCharging
}

type commandResponse struct {
	CommandID string `json:"command_id"`
	ChargerID string `json:"charger_id"`
	Status    string `json:"status"`
	Detail    string `json:"detail"`
	Attempts  int    `json:"attempts"`
}

func (g *mockGateway) handleHealth(w http.ResponseWriter, _ *http.Request) {
	w.Header().Set(mockHeader, "true")
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]string{"status": "ok", "kind": "mock-gateway"})
}

// handleCommand answers POST /chargers/{chargerId}/commands.
//
// The command id is the idempotency key. A repeated request - same id, same charger, same action,
// same order - is answered with the stored outcome and an incremented attempt counter, because a
// retry must not restart a device a second time. The same id with a different charger, action or
// order is a 409: the stored result belongs to the first request, and answering the second one
// would report an outcome for work that was never done (or, for a charge command, attach it to the
// wrong order).
func (g *mockGateway) handleCommand(w http.ResponseWriter, r *http.Request) {
	w.Header().Set(mockHeader, "true")
	if r.Method != http.MethodPost {
		http.Error(w, `{"error":"method not allowed"}`, http.StatusMethodNotAllowed)
		return
	}
	chargerID := strings.TrimPrefix(r.URL.Path, "/chargers/")
	chargerID = strings.TrimSuffix(chargerID, "/commands")
	chargerID = strings.Trim(chargerID, "/")
	if chargerID == "" || strings.Contains(chargerID, "/") {
		http.Error(w, `{"error":"charger id is required"}`, http.StatusBadRequest)
		return
	}

	var request commandRequest
	if err := httpapi.DecodeJSONStrict(http.MaxBytesReader(w, r.Body, 16*1024), &request); err != nil {
		http.Error(w, `{"error":"invalid command body"}`, http.StatusBadRequest)
		return
	}
	commandID := strings.TrimSpace(request.CommandID)
	if commandID == "" {
		http.Error(w, `{"error":"command_id is required"}`, http.StatusBadRequest)
		return
	}
	if header := r.Header.Get("Idempotency-Key"); header != "" && header != commandID {
		http.Error(w, `{"error":"Idempotency-Key does not match command_id"}`, http.StatusBadRequest)
		return
	}
	action := strings.ToUpper(strings.TrimSpace(request.Action))

	// The claim loop owns the whole lifecycle of a command id, and it is a loop because a
	// placeholder can disappear underneath a waiter: when the request that was executing the
	// command goes away before the device answers, it clears its placeholder and wakes the waiters,
	// and each of them then has to re-decide what it is allowed to do. Answering from the state it
	// saw before waking is what produced a fabricated FAILED receipt for a device that had simply
	// not been reached.
	for {
		claim, conflict := g.claim(commandID, chargerID, request.OrderNo, action, request.TraceID)
		if conflict {
			writeCommandError(w, http.StatusConflict, "command_id was already used for a different request", commandID)
			return
		}

		if claim.execute {
			// This request now owns the placeholder, and every early return from here must give it
			// back: an unsupported action used to return before the placeholder was cleared, so the
			// next request with the same id waited forever for an outcome that could never arrive.
			if !supportedActions[action] {
				g.abandon(commandID, "unsupported action")
				// An action outside the frozen set is a client error, which the dispatcher turns
				// into a permanent failure.
				writeCommandError(w, http.StatusBadRequest, "unsupported action", commandID)
				return
			}
			if actionNeedsOrder(action) && strings.TrimSpace(request.OrderNo) == "" {
				g.abandon(commandID, "a charge command needs an order number")
				writeCommandError(w, http.StatusBadRequest, action+" requires order_no", commandID)
				return
			}
			status, detail, ok := g.simulate(r.Context(), commandID, chargerID, request.OrderNo, action, request.TraceID)
			if !ok {
				// The device never answered, so there is no outcome and no response to give.
				return
			}
			writeCommandResponse(w, &commandRecord{
				commandID: commandID, chargerID: chargerID,
				status: status, detail: detail, attempts: 1,
			})
			return
		}

		if claim.waiter {
			// Another delivery of the same command is talking to the device. Wait for it, then
			// re-decide: if it produced an outcome this request replays it, and if it disappeared
			// this request becomes the executor itself.
			select {
			case <-claim.record.done:
				continue
			case <-r.Context().Done():
				return
			}
		}

		// The command already has an outcome: reuse it rather than restarting a device.
		g.mu.Lock()
		claim.record.attempts++
		replayed := *claim.record
		g.mu.Unlock()
		g.logger.Info("device command replayed", "command_id", commandID, "attempts", replayed.attempts)
		// A fact whose report failed is retried by the next delivery of the same command. The device
		// is not asked to do the work again - only the platform is told again what it already did -
		// and the receipt is byte-for-byte the one the first attempt would have sent.
		g.reportReceipt(claim.record)
		writeCommandResponse(w, &replayed)
		return
	}
}

// simulate performs the device work and records the outcome.
//
// It returns ok=false when the request went away before the device answered, in which case the
// placeholder has been cleared and no outcome was stored: a retry must be able to execute the
// command, because nothing may claim a device accepted something it never received.
func (g *mockGateway) simulate(ctx context.Context, commandID, chargerID, orderNo, action, traceID string) (status string, detail string, ok bool) {
	if g.delay > 0 {
		select {
		case <-time.After(g.delay):
		case <-ctx.Done():
			g.abandon(commandID, "the request was cancelled before the device answered")
			return "", "", false
		}
	}

	status = g.result
	if status == "" {
		status = statusCompleted
	}
	if g.failing[chargerID] {
		status = "FAILED"
	}
	detail = "mock gateway outcome"

	g.mu.Lock()
	record, exists := g.commands[commandID]
	if !exists {
		// Somebody abandoned this id while the device was answering. The work did happen, so a
		// fresh record is written rather than dropping the outcome on the floor.
		record = &commandRecord{commandID: commandID, chargerID: chargerID, orderNo: orderNo, action: action, traceID: traceID, done: make(chan struct{})}
		g.commands[commandID] = record
	}
	record.status = status
	record.detail = detail
	record.inProgress = false
	record.attempts = 1
	g.executions++
	g.mu.Unlock()
	close(record.done)

	g.logger.Info("device command handled",
		"command_id", commandID, "charger_id", chargerID,
		"action", action, "status", status, "trace_id", traceID)

	// The device did the work, so the platform is told what it did before the dispatcher is answered:
	// a caller that has been told the device completed the command can then observe the order the
	// fact produced, instead of racing the report it triggered.
	g.reportReceipt(record)
	// A completed start begins a charge, so the simulated meter starts reporting; a completed
	// stop ends it. Failed commands change nothing.
	if status == statusCompleted {
		switch action {
		case actionStartCharging:
			g.startMeterReporting(orderNo, chargerID, traceID)
		case actionStopCharging:
			g.stopMeterReporting(orderNo)
		}
	}
	return status, detail, true
}

// writeCommandError answers with the shared error shape.
func writeCommandError(w http.ResponseWriter, status int, message, commandID string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(map[string]string{
		"error":      message,
		"command_id": commandID,
	})
}

// commandClaim is what one request is allowed to do with a command id.
type commandClaim struct {
	// execute is true for the request that must simulate the device work.
	execute bool
	// waiter is true when another request is already executing this command.
	waiter bool
	// record is the stored record, for the execute and replay paths.
	record *commandRecord
}

// claim resolves the command id in one critical section.
//
// The placeholder is created here, before the device delay, so a duplicate arriving during that
// delay can only wait or conflict - it can never execute the same command again.
func (g *mockGateway) claim(commandID, chargerID, orderNo, action, traceID string) (commandClaim, bool) {
	g.mu.Lock()
	defer g.mu.Unlock()

	record, exists := g.commands[commandID]
	if !exists {
		record = &commandRecord{
			commandID:   commandID,
			chargerID:   chargerID,
			orderNo:     orderNo,
			action:      action,
			traceID:     traceID,
			done:        make(chan struct{}),
			inProgress:  true,
			attempts:    0,
			firstSeenAt: time.Now().UTC(),
		}
		g.commands[commandID] = record
		return commandClaim{execute: true, record: record}, false
	}
	if record.chargerID != chargerID || record.action != action || record.orderNo != orderNo {
		// The stored outcome belongs to the first request; answering this one would report an
		// outcome for work that was never done - and for a charge action it would report it against
		// the wrong order, which is why order_no is part of the identity (BE-I-02 review).
		return commandClaim{}, true
	}
	if record.inProgress {
		return commandClaim{waiter: true, record: record}, false
	}
	return commandClaim{record: record}, false
}

// abandon clears a placeholder whose requester went away, so a retry can execute the command.
func (g *mockGateway) abandon(commandID, detail string) {
	g.mu.Lock()
	record, ok := g.commands[commandID]
	if !ok {
		g.mu.Unlock()
		return
	}
	delete(g.commands, commandID)
	g.mu.Unlock()
	close(record.done)
	g.logger.Warn("device command abandoned without an outcome", "command_id", commandID, "detail", detail)
}

// executionsCount reports how often device work was simulated, for tests and the stop log.
func (g *mockGateway) executionsCount() int {
	g.mu.Lock()
	defer g.mu.Unlock()
	return g.executions
}

func writeCommandResponse(w http.ResponseWriter, record *commandRecord) {
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(commandResponse{
		CommandID: record.commandID,
		ChargerID: record.chargerID,
		Status:    record.status,
		Detail:    record.detail,
		Attempts:  record.attempts,
	})
}

func (g *mockGateway) commandCount() int {
	g.mu.Lock()
	defer g.mu.Unlock()
	return len(g.commands)
}

// executedCount is what the shutdown line reports: how many commands actually reached the (mock)
// device, which is the number a duplicate must not inflate.
func (g *mockGateway) executedCount() int { return g.executionsCount() }

func parseIDSet(raw string) map[string]bool {
	set := map[string]bool{}
	for _, part := range strings.Split(raw, ",") {
		part = strings.TrimSpace(part)
		if part != "" {
			set[part] = true
		}
	}
	return set
}

func envOr(name, fallback string) string {
	if value := strings.TrimSpace(os.Getenv(name)); value != "" {
		return value
	}
	return fallback
}

// boolOr reads a boolean environment switch. Only the values strconv.ParseBool accepts turn a
// switch on: a typo must not silently enable device reporting, so anything else keeps the default.
func boolOr(name string, fallback bool) bool {
	value, err := strconv.ParseBool(strings.TrimSpace(os.Getenv(name)))
	if err != nil {
		return fallback
	}
	return value
}

// int64Or reads an integer environment setting, rejecting a negative or unparsable one rather than
// reporting energy the platform would refuse.
func int64Or(name string, fallback int64) int64 {
	value, err := strconv.ParseInt(strings.TrimSpace(os.Getenv(name)), 10, 64)
	if err != nil || value < 0 {
		if raw := strings.TrimSpace(os.Getenv(name)); raw != "" {
			fmt.Fprintf(os.Stderr, "invalid %s %q, using %d\n", name, raw, fallback)
		}
		return fallback
	}
	return value
}

// floatOr parses an environment/inline numeric value, keeping the fallback when it is unusable:
// the mock is a development tool, and refusing to start over a mistyped simulation knob would be
// worse than simulating with the default.
func floatOr(raw string, fallback float64) float64 {
	value := strings.TrimSpace(raw)
	if value == "" {
		return fallback
	}
	parsed, err := strconv.ParseFloat(value, 64)
	if err != nil {
		return fallback
	}
	return parsed
}

func durationOr(raw string, fallback time.Duration) time.Duration {
	raw = strings.TrimSpace(raw)
	if raw == "" {
		return fallback
	}
	value, err := time.ParseDuration(raw)
	if err != nil || value < 0 {
		fmt.Fprintf(os.Stderr, "invalid duration %q, using %s\n", raw, fallback)
		return fallback
	}
	return value
}
