package main

import (
	"bytes"
	"context"
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

// The mock gateway is the only device the closed-loop verification can talk to, so it has to be
// idempotent in the way a real gateway is: the review found that it overwrote its record for a
// command id and always reported one attempt, which meant a retry could not be distinguished from
// a first delivery - and the whole point of the dispatcher's retry is that it is safe.

func newTestGateway(t *testing.T) (*mockGateway, *httptest.Server) {
	t.Helper()
	gateway := &mockGateway{
		logger:   slog.New(slog.NewTextHandler(io.Discard, nil)),
		result:   "COMPLETED",
		failing:  map[string]bool{},
		commands: map[string]*commandRecord{},
	}
	server := httptest.NewServer(http.HandlerFunc(gateway.handleCommand))
	t.Cleanup(server.Close)
	return gateway, server
}

func postCommand(t *testing.T, server *httptest.Server, chargerID, commandID, action string) (int, commandResponse) {
	t.Helper()
	body, err := json.Marshal(commandRequest{CommandID: commandID, ChargerID: chargerID, Action: action})
	if err != nil {
		t.Fatalf("marshal request: %v", err)
	}
	request, err := http.NewRequestWithContext(context.Background(), http.MethodPost,
		server.URL+"/chargers/"+chargerID+"/commands", bytes.NewReader(body))
	if err != nil {
		t.Fatalf("build request: %v", err)
	}
	request.Header.Set("Content-Type", "application/json")
	request.Header.Set("Idempotency-Key", commandID)

	response, err := server.Client().Do(request)
	if err != nil {
		t.Fatalf("post command: %v", err)
	}
	defer func() { _ = response.Body.Close() }()

	decoded := commandResponse{}
	if response.StatusCode == http.StatusOK {
		if err := json.NewDecoder(response.Body).Decode(&decoded); err != nil {
			t.Fatalf("decode response: %v", err)
		}
	}
	return response.StatusCode, decoded
}

// postCommandWithin posts a command with a bounded wait: a request that never answers must fail the
// test rather than hang it.
func postCommandWithin(t *testing.T, server *httptest.Server, chargerID, commandID, action string, timeout time.Duration) (int, string) {
	t.Helper()
	status, _, body := postCommandFull(t, server, chargerID, commandID, action, timeout)
	return status, body
}

// postChargeCommandWithin posts a charge command with a bounded wait.
func postChargeCommandWithin(t *testing.T, server *httptest.Server, chargerID, commandID, action, orderNo string, timeout time.Duration) (int, string) {
	t.Helper()
	status, _, body := postCommandFullOrder(t, server, chargerID, commandID, action, orderNo, timeout)
	return status, body
}

func postCommandFull(t *testing.T, server *httptest.Server, chargerID, commandID, action string, timeout time.Duration) (int, commandResponse, string) {
	t.Helper()
	return postCommandFullOrder(t, server, chargerID, commandID, action, "", timeout)
}

// postCommandFullOrder is the same request with an explicit order number, because a charge command
// is only valid when it names the order it belongs to.
func postCommandFullOrder(t *testing.T, server *httptest.Server, chargerID, commandID, action, orderNo string, timeout time.Duration) (int, commandResponse, string) {
	t.Helper()
	body, err := json.Marshal(commandRequest{CommandID: commandID, ChargerID: chargerID, OrderNo: orderNo, Action: action})
	if err != nil {
		t.Fatalf("marshal request: %v", err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	request, err := http.NewRequestWithContext(ctx, http.MethodPost,
		server.URL+"/chargers/"+chargerID+"/commands", bytes.NewReader(body))
	if err != nil {
		t.Fatalf("build request: %v", err)
	}
	request.Header.Set("Idempotency-Key", commandID)

	response, err := server.Client().Do(request)
	if err != nil {
		t.Fatalf("post command %s: %v", commandID, err)
	}
	defer func() { _ = response.Body.Close() }()
	raw, err := io.ReadAll(response.Body)
	if err != nil {
		t.Fatalf("read response: %v", err)
	}
	decoded := commandResponse{}
	if response.StatusCode == http.StatusOK {
		if err := json.Unmarshal(raw, &decoded); err != nil {
			t.Fatalf("decode response %s: %v", raw, err)
		}
	}
	return response.StatusCode, decoded, string(raw)
}

// The first delivery is answered with the decided outcome and one attempt.
func TestGatewayAnswersTheFirstCommand(t *testing.T) {
	_, server := newTestGateway(t)
	status, response := postCommand(t, server, "28", "cmd_1", "RESTART")
	if status != http.StatusOK {
		t.Fatalf("status = %d, want 200", status)
	}
	if response.Status != "COMPLETED" || response.CommandID != "cmd_1" || response.ChargerID != "28" {
		t.Fatalf("unexpected response %+v", response)
	}
	if response.Attempts != 1 {
		t.Fatalf("attempts = %d, want 1", response.Attempts)
	}
}

// The review finding: the idempotency check and the record write were separate, so two identical
// requests arriving together both saw "no record yet", both waited out the device delay, both
// executed the command and both wrote the record. This drives exactly that shape.
func TestGatewayExecutesAConcurrentDuplicateOnce(t *testing.T) {
	gateway, server := newTestGateway(t)
	// A device delay wide enough that every request is in flight at the same time.
	gateway.delay = 100 * time.Millisecond

	const concurrent = 6
	type result struct {
		status   int
		response commandResponse
	}
	results := make(chan result, concurrent)
	start := make(chan struct{})
	var wg sync.WaitGroup
	for i := 0; i < concurrent; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			<-start
			status, response := postCommand(t, server, "28", "cmd_concurrent", "RESTART")
			results <- result{status: status, response: response}
		}()
	}
	close(start)
	wg.Wait()
	close(results)

	statuses := map[int]int{}
	attempts := map[int]int{}
	for result := range results {
		statuses[result.status]++
		if result.status == http.StatusOK {
			if result.response.Status != "COMPLETED" {
				t.Fatalf("unexpected status in response: %+v", result.response)
			}
			attempts[result.response.Attempts]++
		}
	}
	if statuses[http.StatusOK] != concurrent {
		t.Fatalf("expected every duplicate to be answered, got %v", statuses)
	}
	// The decisive assertion: device work happened once, however many duplicates arrived.
	if executions := gateway.executionsCount(); executions != 1 {
		t.Fatalf("device work executed %d times for one command id, want 1", executions)
	}
	// Each delivery is counted once, so the attempt numbers are distinct.
	if len(attempts) != concurrent {
		t.Fatalf("expected %d distinct attempt numbers, got %v", concurrent, attempts)
	}
}

// Two concurrent requests that reuse an id for different work: one wins, the other conflicts.
func TestGatewayRejectsAConcurrentConflict(t *testing.T) {
	gateway, server := newTestGateway(t)
	gateway.delay = 100 * time.Millisecond

	statuses := make(chan int, 2)
	var wg sync.WaitGroup
	for _, chargerID := range []string{"28", "29"} {
		wg.Add(1)
		go func(charger string) {
			defer wg.Done()
			status, _ := postCommand(t, server, charger, "cmd_race", "RESTART")
			statuses <- status
		}(chargerID)
	}
	wg.Wait()
	close(statuses)

	seen := map[int]int{}
	for status := range statuses {
		seen[status]++
	}
	if seen[http.StatusOK] != 1 || seen[http.StatusConflict] != 1 {
		t.Fatalf("expected one 200 and one 409, got %v", seen)
	}
	if executions := gateway.executionsCount(); executions != 1 {
		t.Fatalf("device work executed %d times, want 1", executions)
	}
}

// A request that goes away before the device answers leaves no fabricated outcome: a retry must be
// able to execute the command, because nothing may claim the device accepted what it never got.
func TestGatewayAbandonsAnOutcomeWhenTheRequesterDisappears(t *testing.T) {
	gateway, server := newTestGateway(t)
	gateway.delay = 500 * time.Millisecond

	ctx, cancel := context.WithCancel(context.Background())
	body, err := json.Marshal(commandRequest{CommandID: "cmd_abandoned", ChargerID: "28", Action: "RESTART"})
	if err != nil {
		t.Fatalf("marshal request: %v", err)
	}
	request, err := http.NewRequestWithContext(ctx, http.MethodPost, server.URL+"/chargers/28/commands", bytes.NewReader(body))
	if err != nil {
		t.Fatalf("build request: %v", err)
	}
	request.Header.Set("Idempotency-Key", "cmd_abandoned")

	done := make(chan struct{})
	go func() {
		defer close(done)
		response, err := server.Client().Do(request)
		if err == nil {
			_ = response.Body.Close()
		}
	}()
	time.Sleep(50 * time.Millisecond)
	cancel()
	<-done

	// The placeholder is cleared once the handler notices the cancellation.
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if gateway.commandCount() == 0 {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	if executions := gateway.executionsCount(); executions != 0 {
		t.Fatalf("a cancelled request must not record a device outcome, executions = %d", executions)
	}

	// The retry executes the command for real.
	gateway.mu.Lock()
	gateway.delay = 0
	gateway.mu.Unlock()
	status, response := postCommand(t, server, "28", "cmd_abandoned", "RESTART")
	if status != http.StatusOK || response.Status == "" {
		t.Fatalf("expected the retry to be answered, got %d %+v", status, response)
	}
	if executions := gateway.executionsCount(); executions != 1 {
		t.Fatalf("expected the retry to execute the command once, executions = %d", executions)
	}
}

// A retry of the same command is answered with the stored outcome, and the attempt counter really
// counts the deliveries.
func TestGatewayReplaysTheSameCommand(t *testing.T) {
	_, server := newTestGateway(t)
	_, first := postCommand(t, server, "28", "cmd_retry", "RESTART")

	for attempt := 2; attempt <= 3; attempt++ {
		status, response := postCommand(t, server, "28", "cmd_retry", "RESTART")
		if status != http.StatusOK {
			t.Fatalf("retry %d status = %d, want 200", attempt, status)
		}
		if response.Status != first.Status {
			t.Fatalf("retry %d answered %q, want the stored %q", attempt, response.Status, first.Status)
		}
		if response.Attempts != attempt {
			t.Fatalf("retry %d attempts = %d, want %d", attempt, response.Attempts, attempt)
		}
	}
}

// The review finding: a reused command id with a different request silently overwrote the record.
// It is a conflict, because the stored outcome belongs to the first request.
func TestGatewayRejectsAReusedCommandIDWithADifferentRequest(t *testing.T) {
	cases := map[string]struct {
		chargerID string
		action    string
	}{
		"different charger": {chargerID: "29", action: "RESTART"},
		"different action":  {chargerID: "28", action: "STOP_CHARGING"},
	}
	for name, request := range cases {
		t.Run(name, func(t *testing.T) {
			_, server := newTestGateway(t)
			if status, _ := postCommand(t, server, "28", "cmd_conflict", "RESTART"); status != http.StatusOK {
				t.Fatalf("first request status = %d, want 200", status)
			}
			status, _ := postCommand(t, server, request.chargerID, "cmd_conflict", request.action)
			if status != http.StatusConflict {
				t.Fatalf("status = %d, want 409 for a reused command id", status)
			}
		})
	}
}

// The BE-I-02 review finding: once the charge actions carried an order number, that number became
// part of the request's identity. A command id reused for the same charger and the same action but
// a DIFFERENT order was still answered with the first outcome, so a device verdict could be
// attached to the wrong order. It is a conflict like any other changed field.
func TestGatewayRejectsAReusedCommandIDForADifferentOrder(t *testing.T) {
	gateway, server := newTestGateway(t)

	status, _ := postChargeCommandWithin(t, server, "28", "cmd_order_conflict", "START_CHARGING", "ORD-A", 3*time.Second)
	if status != http.StatusOK {
		t.Fatalf("first request status = %d, want 200", status)
	}
	// The same id, charger and action, a different order: 409, and no second execution.
	status, body := postChargeCommandWithin(t, server, "28", "cmd_order_conflict", "START_CHARGING", "ORD-B", 3*time.Second)
	if status != http.StatusConflict {
		t.Fatalf("status = %d (%s), want 409 for a reused command id with a different order", status, body)
	}
	if executions := gateway.executionsCount(); executions != 1 {
		t.Fatalf("device work executed %d times, want 1", executions)
	}

	// The order number is also part of the retry identity: the same order still replays.
	status, _ = postChargeCommandWithin(t, server, "28", "cmd_order_conflict", "START_CHARGING", "ORD-A", 3*time.Second)
	if status != http.StatusOK {
		t.Fatalf("retry status = %d, want 200 for the original order", status)
	}
}

// A station-level RESTART carries no order number, so two identical restarts under one id stay the
// same request: adding order_no to the identity must not turn a legitimate retry into a conflict.
func TestGatewayStillReplaysACommandWithoutAnOrder(t *testing.T) {
	gateway, server := newTestGateway(t)

	if status, _ := postCommand(t, server, "28", "cmd_no_order_retry", "RESTART"); status != http.StatusOK {
		t.Fatalf("first request status = %d, want 200", status)
	}
	status, response := postCommand(t, server, "28", "cmd_no_order_retry", "RESTART")
	if status != http.StatusOK || response.Attempts != 2 {
		t.Fatalf("retry = %d / attempts %d, want 200 and 2", status, response.Attempts)
	}
	if executions := gateway.executionsCount(); executions != 1 {
		t.Fatalf("device work executed %d times, want 1", executions)
	}
}

// The gateway records the outcome it decided, so a later change of the default result does not
// rewrite history for an already answered command.
func TestGatewayKeepsTheFirstOutcomeForACommand(t *testing.T) {
	gateway, server := newTestGateway(t)
	_, first := postCommand(t, server, "28", "cmd_stable", "RESTART")

	gateway.mu.Lock()
	gateway.result = "FAILED"
	gateway.mu.Unlock()

	status, replay := postCommand(t, server, "28", "cmd_stable", "RESTART")
	if status != http.StatusOK {
		t.Fatalf("status = %d, want 200", status)
	}
	if replay.Status != first.Status {
		t.Fatalf("replay answered %q, want the stored %q", replay.Status, first.Status)
	}
	if replay.Attempts != 2 {
		t.Fatalf("attempts = %d, want 2", replay.Attempts)
	}
}

// A charger configured to fail reports FAILED, which is what the verification uses to force the
// failure path.
func TestGatewayReportsAFailingChargerAsFailed(t *testing.T) {
	gateway, server := newTestGateway(t)
	gateway.failing["29"] = true

	status, response := postCommand(t, server, "29", "cmd_failed", "RESTART")
	if status != http.StatusOK {
		t.Fatalf("status = %d, want 200", status)
	}
	if response.Status != "FAILED" {
		t.Fatalf("status = %q, want FAILED", response.Status)
	}
}

// Malformed and unsupported requests are client errors, which the dispatcher turns into permanent
// failures rather than retries.
func TestGatewayRejectsBadRequests(t *testing.T) {
	_, server := newTestGateway(t)
	if status, _ := postCommand(t, server, "28", "cmd_noaction", ""); status != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400 for a missing action", status)
	}
	if status, _ := postCommand(t, server, "", "cmd_nocharger", "RESTART"); status == http.StatusOK {
		t.Fatal("expected a missing charger id to be rejected")
	}
	for _, body := range []string{
		`{"command_id":"cmd_unknown","charger_id":"28","action":"RESTART","force":true}`,
		`{"command_id":"cmd_trailing","charger_id":"28","action":"RESTART"} {}`,
	} {
		request, err := http.NewRequestWithContext(context.Background(), http.MethodPost,
			server.URL+"/chargers/28/commands", strings.NewReader(body))
		if err != nil {
			t.Fatalf("build request: %v", err)
		}
		response, err := server.Client().Do(request)
		if err != nil {
			t.Fatalf("post invalid command: %v", err)
		}
		_ = response.Body.Close()
		if response.StatusCode != http.StatusBadRequest {
			t.Errorf("body %q: status = %d, want 400", body, response.StatusCode)
		}
	}
}

// A rejected request must not leave its placeholder behind. The first version validated the action
// after claiming, so a new command id with an unsupported action answered 400 and kept the
// placeholder: the next identical request waited for an outcome that could never arrive. The
// review reproduced that as "first request 400, second request times out".
func TestGatewayRejectsAnUnsupportedActionTwiceWithoutLeavingAPlaceholder(t *testing.T) {
	gateway, server := newTestGateway(t)

	for attempt := 1; attempt <= 2; attempt++ {
		status, body := postCommandWithin(t, server, "28", "cmd_bad_action", "STOP_CHARGING", 2*time.Second)
		if status != http.StatusBadRequest {
			t.Fatalf("attempt %d: status = %d, body = %s; want 400", attempt, status, body)
		}
	}
	// No record may survive a rejected request.
	if records := gateway.commandCount(); records != 0 {
		t.Fatalf("a rejected request left %d record(s) behind", records)
	}
	if executions := gateway.executionsCount(); executions != 0 {
		t.Fatalf("a rejected request executed device work %d time(s)", executions)
	}

	// And the id is still usable: the rejection must not wedge it.
	status, body := postCommandWithin(t, server, "28", "cmd_bad_action", "RESTART", 3*time.Second)
	if status != http.StatusOK {
		t.Fatalf("a valid request for the same id answered %d (%s), want 200", status, body)
	}
	if executions := gateway.executionsCount(); executions != 1 {
		t.Fatalf("expected the valid request to execute once, got %d", executions)
	}
}

// The mock gateway is the only device the closed-loop verification can talk to (BE-I-02), so it has
// to accept the two charge actions exactly as the frozen contract describes them and reject a charge
// command that names no order - the dispatcher refuses to send one, and the device must agree.
func TestGatewayAcceptsAChargeCommandWithItsOrder(t *testing.T) {
	gateway, server := newTestGateway(t)

	for _, action := range []string{"START_CHARGING", "STOP_CHARGING"} {
		commandID := "cmd_" + strings.ToLower(action)
		status, body := postChargeCommandWithin(t, server, "28", commandID, action, "ORD20240101001", 3*time.Second)
		if status != http.StatusOK {
			t.Fatalf("%s: status = %d (%s), want 200", action, status, body)
		}
	}

	if executions := gateway.executionsCount(); executions != 2 {
		t.Fatalf("expected both charge commands to reach the device, executions = %d", executions)
	}
}

// The same class of bug the unsupported-action review found: a rejected request that keeps its
// placeholder wedges the command id and makes the retry wait for an outcome that can never arrive.
// Without the abandon before the 400 the second request would hang, which the bounded wait detects.
func TestGatewayRejectsAChargeCommandWithoutAnOrderTwiceWithoutLeavingAPlaceholder(t *testing.T) {
	for _, action := range []string{"START_CHARGING", "STOP_CHARGING"} {
		t.Run(action, func(t *testing.T) {
			gateway, server := newTestGateway(t)
			commandID := "cmd_no_order"

			for attempt := 1; attempt <= 2; attempt++ {
				status, body := postChargeCommandWithin(t, server, "28", commandID, action, "", 2*time.Second)
				if status != http.StatusBadRequest {
					t.Fatalf("attempt %d: status = %d (%s), want 400", attempt, status, body)
				}
			}
			if records := gateway.commandCount(); records != 0 {
				t.Fatalf("a rejected request left %d record(s) behind", records)
			}
			if executions := gateway.executionsCount(); executions != 0 {
				t.Fatalf("a rejected request executed device work %d time(s)", executions)
			}

			// The id stays usable once the command is valid, so a rejection never wedges it.
			status, body := postChargeCommandWithin(t, server, "28", commandID, action, "ORD20240101001", 3*time.Second)
			if status != http.StatusOK {
				t.Fatalf("a valid request for the same id answered %d (%s), want 200", status, body)
			}
			if executions := gateway.executionsCount(); executions != 1 {
				t.Fatalf("expected the valid request to execute once, got %d", executions)
			}
		})
	}
}

// A waiter must never be told a device failed when the device was never reached. The first version
// woke the waiter, found no record (the first request had abandoned it) and answered 200 with a
// fabricated FAILED. The waiter now re-claims and executes the command itself.
func TestGatewayLetsAWaitersReExecuteWhenTheFirstRequestIsCancelled(t *testing.T) {
	gateway, server := newTestGateway(t)
	gateway.delay = 400 * time.Millisecond

	// The first delivery is cancelled while it is waiting for the device.
	firstCtx, cancelFirst := context.WithCancel(context.Background())
	firstDone := make(chan struct{})
	go func() {
		defer close(firstDone)
		body := []byte(`{"command_id":"cmd_handover","charger_id":"28","action":"RESTART"}`)
		request, err := http.NewRequestWithContext(firstCtx, http.MethodPost,
			server.URL+"/chargers/28/commands", bytes.NewReader(body))
		if err != nil {
			return
		}
		request.Header.Set("Idempotency-Key", "cmd_handover")
		response, err := server.Client().Do(request)
		if err == nil {
			_ = response.Body.Close()
		}
	}()

	// The second delivery arrives while the first is still waiting: it is a waiter.
	time.Sleep(50 * time.Millisecond)
	type result struct {
		status   int
		response commandResponse
		body     string
	}
	second := make(chan result, 1)
	go func() {
		status, response, body := postCommandFull(t, server, "28", "cmd_handover", "RESTART", 5*time.Second)
		second <- result{status: status, response: response, body: body}
	}()
	time.Sleep(50 * time.Millisecond)

	// The first delivery goes away before the device answers.
	cancelFirst()
	<-firstDone

	select {
	case got := <-second:
		if got.status != http.StatusOK {
			t.Fatalf("the waiter answered %d (%s), want 200 with a real outcome", got.status, got.body)
		}
		if got.response.Status != "COMPLETED" {
			t.Fatalf("the waiter answered status %q (%s); a device that was never reached must not be reported as failed",
				got.response.Status, got.body)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("the waiter never answered")
	}

	// The command reached the device exactly once, by the delivery that took it over.
	if executions := gateway.executionsCount(); executions != 1 {
		t.Fatalf("device work executed %d times, want 1", executions)
	}
}

// The health endpoint is what a deployment probes, and it says what this process is.
func TestGatewayHealthSaysItIsAMock(t *testing.T) {
	gateway, server := newTestGateway(t)
	mux := http.NewServeMux()
	mux.HandleFunc("/healthz", gateway.handleHealth)
	probe := httptest.NewServer(mux)
	t.Cleanup(probe.Close)

	_ = server
	response, err := probe.Client().Get(probe.URL + "/healthz")
	if err != nil {
		t.Fatalf("health request: %v", err)
	}
	defer func() { _ = response.Body.Close() }()
	if response.Header.Get(mockHeader) != "true" {
		t.Fatal("every response must be marked as coming from the mock gateway")
	}
	if response.StatusCode != http.StatusOK {
		t.Fatalf("health status = %d, want 200", response.StatusCode)
	}
}
