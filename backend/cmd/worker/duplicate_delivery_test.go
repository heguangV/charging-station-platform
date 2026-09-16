package main

import (
	"context"
	"os"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/heguangV/charging-station-platform/backend/internal/event"
	"github.com/heguangV/charging-station-platform/backend/internal/repository/postgres"
	redisrepo "github.com/heguangV/charging-station-platform/backend/internal/repository/redis"
	"github.com/heguangV/charging-station-platform/backend/internal/worker"
	"github.com/heguangV/charging-station-platform/backend/migrations"
)

// This is the duplicate-delivery check the review asked for, at the level where duplication
// actually happens: the same event delivered twice by Redis Streams, consumed by a real worker
// over the real PostgreSQL consumption store.
//
// It is the property the whole module exists to make true. The in-memory store could not provide
// it (a restart lost the record), and a test that only asserts it against a fake store would say
// nothing about the deployment.

// countingApplier records how often the domain application ran.
type countingApplier struct {
	mu    sync.Mutex
	calls int
	seen  []string
}

func (a *countingApplier) Apply(_ context.Context, e event.Event, _ int) error {
	a.mu.Lock()
	defer a.mu.Unlock()
	a.calls++
	a.seen = append(a.seen, e.EventID)
	return nil
}

func (a *countingApplier) applications() int {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.calls
}

func requireDuplicateDeliveryEnvironment(t *testing.T) (context.Context, *redisrepo.Capabilities, string) {
	t.Helper()
	dsn := strings.TrimSpace(os.Getenv("NCS_TEST_PG_DSN"))
	address := strings.TrimSpace(os.Getenv("NCS_REDIS_TEST_ADDR"))
	if dsn == "" || address == "" {
		t.Skip("NCS_TEST_PG_DSN and NCS_REDIS_TEST_ADDR are required; duplicate delivery is asserted against the real stores")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	t.Cleanup(cancel)

	settings := redisrepo.DefaultSettings()
	settings.Conn.Address = address
	if raw := strings.TrimSpace(os.Getenv("NCS_REDIS_TEST_DB")); raw != "" {
		database, err := strconv.Atoi(raw)
		if err != nil {
			t.Fatalf("invalid NCS_REDIS_TEST_DB=%q: %v", raw, err)
		}
		settings.Conn.Database = database
	}
	capabilities, err := redisrepo.NewCapabilities(settings)
	if err != nil {
		t.Fatalf("create redis capabilities: %v", err)
	}
	t.Cleanup(func() { _ = capabilities.Close() })
	return ctx, capabilities, dsn
}

func TestDuplicateDeliveryAppliesTheEventOnce(t *testing.T) {
	ctx, capabilities, dsn := requireDuplicateDeliveryEnvironment(t)

	db, err := postgres.Open(ctx, dsn, 4)
	if err != nil {
		t.Fatalf("open postgres: %v", err)
	}
	t.Cleanup(func() { _ = db.Close() })
	// Coordinate with schema-rebuilding repository tests sharing this disposable DB.
	conn, err := db.Conn(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := conn.ExecContext(ctx, `SELECT pg_advisory_lock($1)`, int64(0x4E43535F54455354)); err != nil {
		_ = conn.Close()
		t.Fatal(err)
	}
	t.Cleanup(func() {
		_, _ = conn.ExecContext(context.Background(), `SELECT pg_advisory_unlock($1)`, int64(0x4E43535F54455354))
		_ = conn.Close()
	})
	runnerDB, err := postgres.NewSQLDB(db)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := postgres.Run(ctx, runnerDB, migrations.FS); err != nil {
		t.Fatal(err)
	}
	store, err := postgres.NewConsumptionStore(db)
	if err != nil {
		t.Fatalf("new consumption store: %v", err)
	}

	suffix := strconv.FormatInt(time.Now().UnixNano(), 10)
	stream := "ncs:test:duplicate:" + suffix
	group := "duplicate-consumers"
	// The stream is left behind on purpose: deleting it while a reader might still hold it would
	// race, and it is unique per run.
	applier := &countingApplier{}
	handler, err := worker.NewChargeHandler(applier, store, nil, worker.ChargeHandlerConfig{
		Consumer: "duplicate-consumer",
		Scope:    "duplicate-delivery-" + suffix,
	})
	if err != nil {
		t.Fatalf("new charge handler: %v", err)
	}
	router := worker.NewRouter()
	if err := router.RegisterAll(worker.DefaultChargeEventTypes(), handler); err != nil {
		t.Fatalf("register handler: %v", err)
	}

	config := worker.Config{
		Stream:          stream,
		Group:           group,
		Consumer:        "duplicate-consumer",
		Count:           10,
		Block:           50 * time.Millisecond,
		PendingInterval: time.Hour,
		RetryAfter:      time.Hour,
		MaxAttempts:     3,
	}
	w, err := worker.New(capabilities.Streams, router, config)
	if err != nil {
		t.Fatalf("new worker: %v", err)
	}
	recorder, err := worker.NewDeadLetterRecorder(store)
	if err != nil {
		t.Fatalf("new dead-letter recorder: %v", err)
	}
	w.SetDeadLetterRecorder(recorder)

	// One event, published twice with the same identity - which is exactly what the window
	// between the stream append and the published mark produces in production.
	e, err := event.New(event.ChargeStartRequested, "order", "order_duplicate", "trace-duplicate",
		map[string]string{"userId": "1"})
	if err != nil {
		t.Fatalf("new event: %v", err)
	}
	e.EventID = "evt_duplicate_" + suffix
	fields, err := e.Fields()
	if err != nil {
		t.Fatalf("event fields: %v", err)
	}
	for i := 0; i < 2; i++ {
		if _, err := capabilities.Streams.Add(ctx, stream, fields); err != nil {
			t.Fatalf("publish copy %d: %v", i, err)
		}
	}

	runCtx, cancel := context.WithCancel(ctx)
	done := make(chan error, 1)
	go func() { done <- w.Run(runCtx) }()

	deadline := time.Now().Add(20 * time.Second)
	var pending []redisrepo.PendingMessage
	for time.Now().Before(deadline) {
		pending, err = capabilities.Streams.Pending(ctx, stream, group)
		if err == nil && len(pending) == 0 {
			// Both copies have been read and acknowledged; give the second one a moment to be
			// reported as a duplicate before asserting.
			time.Sleep(50 * time.Millisecond)
			pending, _ = capabilities.Streams.Pending(ctx, stream, group)
			if len(pending) == 0 {
				break
			}
		}
		time.Sleep(20 * time.Millisecond)
	}
	cancel()
	if err := <-done; err != nil {
		t.Fatalf("worker stopped with error: %v", err)
	}
	if len(pending) != 0 {
		t.Fatalf("expected both deliveries to be acknowledged, %d still pending", len(pending))
	}

	// The decisive assertion: the domain application ran once, even though the event was
	// delivered twice.
	if applications := applier.applications(); applications != 1 {
		t.Fatalf("expected the event to be applied exactly once, applied %d times: %v", applications, applier.seen)
	}
	entry, ok, err := store.Entry(ctx, e.EventID)
	if err != nil {
		t.Fatalf("read consumption record: %v", err)
	}
	if !ok {
		t.Fatal("expected a consumption record for the duplicated event")
	}
	if entry.Outcome != event.OutcomeSucceeded {
		t.Fatalf("expected the terminal success outcome, got %s", entry.Outcome)
	}
	// A duplicate that was misread as a failure would be parked. The shared Redis database may hold
	// dead letters from other suites, so the assertion is on the record: a wrongly-classified
	// duplicate would have replaced the success with a terminal dead-letter outcome.
	if !entry.Outcome.Terminal() {
		t.Fatal("expected a terminal outcome")
	}
}
