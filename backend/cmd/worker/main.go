package main

import (
	"context"
	"database/sql"
	"flag"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"github.com/heguangV/charging-station-platform/backend/internal/config"
	"github.com/heguangV/charging-station-platform/backend/internal/event"
	"github.com/heguangV/charging-station-platform/backend/internal/observability"
	"github.com/heguangV/charging-station-platform/backend/internal/order"
	"github.com/heguangV/charging-station-platform/backend/internal/repository/postgres"
	redisrepo "github.com/heguangV/charging-station-platform/backend/internal/repository/redis"
	"github.com/heguangV/charging-station-platform/backend/internal/worker"
	"github.com/heguangV/charging-station-platform/backend/migrations"
)

// This executable wires the B-line event workers: charge and order lifecycle events
// plus charger device commands, consumed from real Redis Streams through the pooled
// RESP2 client.
//
// The process is fully wired (BE-I-01): the consumption record and the order domain come
// from PostgreSQL, the duplicate guard and the streams come from Redis, the device command
// goes to the charger gateway over HTTP, and the device outcome is recorded with the
// completion event it produces.
//
// The gateway address is required rather than defaulted: a dispatcher that falls back to a
// development address would send real restart commands to a mock, so a deployment that
// forgot to configure one refuses to start.
func main() {
	var extraStreams string
	flag.StringVar(&extraStreams, "streams", envOr("NCS_WORKER_STREAMS", ""), "comma separated stream=group entries overriding the defaults; a stream=group@start_id entry also sets the position a new consumer group starts from (default 0-0, \"$\" ignores existing history)")
	flag.Parse()

	// The context handler adds the trace id from the context to every record, so one
	// wrapper makes every line this process writes - including the workers' lines -
	// searchable by the originating business request.
	logger := slog.New(observability.NewContextHandler(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelInfo})))

	settings, err := redisrepo.SettingsFromEnv(nil)
	if err != nil {
		logger.Error("load redis configuration", "error", err)
		os.Exit(1)
	}
	logger.Info("redis configuration loaded",
		"endpoint", settings.Conn.String(),
		"policy", settings.PolicySummary(),
		"required", settings.Required,
	)

	capabilities, err := redisrepo.NewCapabilities(settings)
	if err != nil {
		logger.Error("create redis foundation", "error", err)
		os.Exit(1)
	}
	defer func() {
		if err := capabilities.Close(); err != nil {
			logger.Error("close redis foundation", "error", err)
		}
	}()

	// The metrics registry is the single source of the reliability facts this process
	// reports: worker counters and sampled stream state land in the same place, so one scrape -
	// and one log line - describes the whole pipeline.
	registry := observability.NewRegistry()
	successClock := observability.NewSuccessClock(registry, observability.MetricWorkerLastSuccess)
	// The ruling lists the metrics a scrape must find; the fixed ones are created here so a fresh
	// process does not look like a build that exports nothing.
	observability.RegisterProcessMetrics(registry, observability.ProcessMetricsConfig{
		Worker:  true,
		Streams: streamNames(),
	})

	// The worker's counters are only visible from the worker, so it serves its own endpoint. The
	// address defaults to loopback and is validated before it is bound: a public bind has to be
	// asked for explicitly (B-06 ruling on ops endpoints).
	metrics, err := observability.StartMetricsServer(observability.MetricsServerConfig{
		Addr:            envOr(metricsAddrEnv, defaultMetricsAddr),
		Registry:        registry,
		Logger:          logger,
		AllowPublicBind: truthyEnv(metricsAllowPublicEnv),
	})
	if err != nil {
		logger.Error("start metrics endpoint", "error", err)
		os.Exit(1)
	}
	defer func() {
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		if err := metrics.Shutdown(shutdownCtx); err != nil {
			logger.Error("shutdown metrics endpoint", "error", err)
		}
	}()

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	// Session and lock default to FailClosed, so a process that cannot reach Redis
	// cannot honour its own guarantees; refusing to start is the approved behaviour.
	probeCtx, cancelProbe := context.WithTimeout(ctx, 5*time.Second)
	readyErr := capabilities.Ready(probeCtx)
	cancelProbe()
	if readyErr != nil {
		if settings.Required {
			logger.Error("redis is unreachable and NCS_REDIS_REQUIRED is true; refusing to start", "error", readyErr)
			os.Exit(1)
		}
		logger.Warn("redis is unreachable; running degraded because NCS_REDIS_REQUIRED is false",
			"error", readyErr,
			"unavailable_guarantees", "session verification, distributed locking and duplicate suppression are unavailable",
		)
	} else {
		logger.Info("redis reachable", "latency", capabilities.Health.Status(ctx).Latency.String())
	}

	consumer := envOr("NCS_WORKER_CONSUMER", "worker-"+fmt.Sprint(os.Getpid()))

	// The sampling interval is configurable because the right cadence differs between a
	// quiet deployment and one being watched during an incident.
	sampleInterval, err := durationOr(envOr("NCS_WORKER_SAMPLE_INTERVAL", ""), 30*time.Second)
	if err != nil {
		logger.Error("invalid NCS_WORKER_SAMPLE_INTERVAL", "error", err)
		os.Exit(1)
	}

	// The consumption record is the authoritative duplicate protection (contract 4.3), so it
	// lives in PostgreSQL. There is deliberately no in-memory fallback: a process that cannot
	// record consumption cannot honour the guarantee, and pretending otherwise would apply an
	// event twice after a restart.
	appConfig, err := config.Load()
	if err != nil {
		logger.Error("load configuration", "error", err)
		os.Exit(1)
	}
	if strings.TrimSpace(appConfig.PostgresDSN) == "" {
		logger.Error("NCS_POSTGRES_DSN is required: the consumption record and the order domain live in PostgreSQL")
		os.Exit(1)
	}
	db, err := postgres.Open(ctx, appConfig.PostgresDSN, 8)
	if err != nil {
		logger.Error("connect to postgres", "error", err)
		os.Exit(1)
	}
	defer func() { _ = db.Close() }()

	// Migration gate: this process writes tables a migration introduces, and it does not run
	// migrations itself. Starting against an older schema would fail later, in the middle of a
	// device flow, instead of here where the fix is one command.
	expectedSchema, err := postgres.HighestMigrationVersion(migrations.FS)
	if err != nil {
		logger.Error("read migration set", "error", err)
		os.Exit(1)
	}
	if err := postgres.AssertSchemaVersion(ctx, db, expectedSchema); err != nil {
		logger.Error("schema is out of date; run the migration gate first", "error", err,
			"expected_schema_version", expectedSchema)
		os.Exit(1)
	}
	if version, err := postgres.SchemaVersion(ctx, db); err == nil {
		logger.Info("schema version verified", "schema_version", version, "expected", expectedSchema)
	}

	consumptionStore, err := postgres.NewConsumptionStore(db)
	if err != nil {
		logger.Error("create consumption store", "error", err)
		os.Exit(1)
	}
	logger.Info("consumption store ready", "backend", "postgresql")

	gatewayURL := strings.TrimSpace(envOr("NCS_CHARGER_GATEWAY_URL", ""))
	if gatewayURL == "" {
		logger.Error("NCS_CHARGER_GATEWAY_URL is required: the command dispatcher must reach a real gateway")
		os.Exit(1)
	}

	router, err := buildRouter(capabilities, consumptionStore, ordersOf(db), gatewayURL, logger)
	if err != nil {
		logger.Error("configure event routing", "error", err)
		os.Exit(1)
	}
	logger.Info("event routing configured", "routes", strings.Join(router.Types(), ","))

	streams, err := buildStreams(consumer, extraStreams)
	if err != nil {
		logger.Error("configure streams", "error", err)
		os.Exit(1)
	}

	runner, err := worker.NewRunner(capabilities.Streams, router, streams)
	if err != nil {
		logger.Error("create worker runner", "error", err)
		os.Exit(1)
	}
	// Dead letters also update the consumption record, so every outcome is persisted
	// rather than only the successful ones.
	deadLetterRecorder, err := worker.NewDeadLetterRecorder(consumptionStore)
	if err != nil {
		logger.Error("create dead-letter recorder", "error", err)
		os.Exit(1)
	}
	runner.SetDeadLetterRecorder(deadLetterRecorder)
	// The dead-letter write is made idempotent per event, so a retry after a partial failure
	// does not park a second copy of the same event. The claim store carries the state, because
	// "somebody is writing this event right now" and "a dead letter for this event exists" demand
	// opposite actions from the next delivery.
	deadLetterGuard, err := worker.NewDeadLetterGuard(capabilities.DeadLetterClaims)
	if err != nil {
		logger.Error("create dead-letter guard", "error", err)
		os.Exit(1)
	}
	runner.SetDeadLetterGuard(deadLetterGuard)
	// The success clock is wrapped around the registry so the timestamp travels the same path as
	// the counters: a worker that is connected but consuming nothing is then visible as a stale
	// timestamp rather than as silence.
	runner.SetObserver(successClock.MarkObserver(registry))
	// The workers share this process's trace-aware logger.
	runner.SetLogger(logger)
	runner.SetOnStart(func(stream worker.StreamConfig) {
		logger.Info("stream worker started", "stream", stream.Stream, "group", stream.Group, "consumer", stream.Consumer)
	})

	// Stream state is sampled periodically because lag and pending depth cannot be derived
	// from the worker's own counters: a counter can say "we are retrying", only the stream
	// state can say how much work is queued behind us.
	targets := make([]observability.StreamTarget, 0, len(streams))
	for _, stream := range streams {
		targets = append(targets, observability.StreamTarget{Stream: stream.Stream, Group: stream.Group})
	}
	collector, err := observability.NewCollector(capabilities.Inspector, registry, targets, event.StreamDeadLetter)
	if err != nil {
		logger.Error("create stream collector", "error", err)
		os.Exit(1)
	}
	// The degradation policy is fail-open or fail-closed per capability, so a failing Redis can
	// be invisible from the outside: the cache stops caching, the duplicate guard stops
	// guarding, and every other metric still looks healthy. Bridging those counters is what makes
	// the policy observable instead of merely implemented.
	degradationSampler, err := observability.NewDegradationSampler(capabilities.Stats, registry)
	if err != nil {
		logger.Error("create degradation sampler", "error", err)
		os.Exit(1)
	}
	collector.SetDegradationSampler(degradationSampler)

	// Dependency sampling runs for as long as the process does. It has its own context so shutdown
	// stops it deterministically instead of waiting for the next tick.
	metricsProbeCtx, stopMetricsProbe := context.WithCancel(ctx)
	metricsProbeDone := make(chan struct{})
	go func() {
		defer close(metricsProbeDone)
		probeDependencies(metricsProbeCtx, db, capabilities.Ready, registry, logger)
	}()
	defer func() {
		stopMetricsProbe()
		<-metricsProbeDone
	}()

	// The startup snapshot is taken once the runner reports that every consumer group exists.
	// Sampling before that cannot report a meaningful backlog: lag is undefined without a group, so a
	// first deployment with a stream full of unconsumed events would have looked healthy.
	runner.SetOnReady(func() {
		if err := collector.Collect(ctx); err != nil {
			logger.Warn("initial stream sampling failed", "error", err.Error())
		}
		logger.Info("stream state at startup", "metrics", collector.Summary())
	})

	runErr := servePipeline(ctx, runner, collector, sampleInterval, logger)
	if runErr != nil {
		logger.Error("worker stopped with error", "error", runErr)
		os.Exit(1)
	}
	logger.Info("worker stopped")
}

// shutdownSampleTimeout bounds the final sampling pass. It runs after the workers have stopped, so
// it must not be able to hold the process open: an unreachable Redis would otherwise keep an
// operator waiting for a process that has nothing left to do.
const shutdownSampleTimeout = 5 * time.Second

// servePipeline runs the workers and the periodic stream sampler together.
//
// It returns once consumption has stopped and the sampler with it, so the caller can close the
// connection pool immediately afterwards without racing an in-flight command.
//
// The sampler gets its own cancellable context rather than the process signal context. A worker
// failure cancels the runner but not the signal, so a sampler that only ever stops on a signal
// would leave the process unable to exit - a state a supervisor cannot tell from a hung process.
// streamNames lists the streams this worker consumes, for the per-stream gauges. It is derived from
// the same frozen list the consumers are built from, so the two cannot drift.
func streamNames() []string {
	return []string{event.StreamOrderEvent, event.StreamChargeEvent, event.StreamChargerCommand}
}

// probeDependencies keeps the process's view of PostgreSQL and Redis current.
//
// The worker has no readiness endpoint (nothing routes traffic to it), but a scrape still has to be
// able to answer "is this process able to work right now": a worker whose PostgreSQL connection is
// gone is holding messages it cannot apply, which is exactly the state the gauges describe.
func probeDependencies(ctx context.Context, db *sql.DB, redisPing func(context.Context) error, registry *observability.Registry, logger *slog.Logger) {
	observability.ProbeDependencies(ctx, observability.ProbeConfig{
		PostgresUp:      func(ctx context.Context) bool { return db.PingContext(ctx) == nil },
		RedisUp:         func(ctx context.Context) bool { return redisPing(ctx) == nil },
		SchemaVersion:   func(ctx context.Context) (int, error) { return postgres.SchemaVersion(ctx, db) },
		OutboxBacklog:   func(ctx context.Context) (int64, error) { return postgres.OutboxBacklog(ctx, db) },
		OutboxOldestAge: func(ctx context.Context) (float64, error) { return postgres.OldestUnpublishedOutboxAge(ctx, db) },
		Registry:        registry,
		Logger:          logger,
		Interval:        dependencyProbeInterval,
	})
}

// The ops endpoint defaults (B-06 ruling): the API is 9090, the worker 9091, the publisher 9092, all
// on loopback, all overridable, none of them hardcoded to a public interface.
const (
	metricsAddrEnv          = "NCS_METRICS_ADDR"
	metricsAllowPublicEnv   = "NCS_METRICS_ALLOW_PUBLIC_BIND"
	defaultMetricsAddr      = "127.0.0.1:9091"
	dependencyProbeInterval = 5 * time.Second
)

func truthyEnv(name string) bool {
	switch strings.ToLower(strings.TrimSpace(os.Getenv(name))) {
	case "1", "true", "yes", "on":
		return true
	}
	return false
}

func servePipeline(ctx context.Context, runner *worker.Runner, collector *observability.Collector, sampleInterval time.Duration, logger *slog.Logger) error {
	reportingCtx, stopReporting := context.WithCancel(ctx)
	reportingDone := make(chan struct{})
	go func() {
		defer close(reportingDone)
		if err := collector.Run(reportingCtx, sampleInterval, logger); err != nil {
			logger.Error("stream sampling stopped", "error", err)
		}
	}()

	runErr := runner.Run(ctx)
	stopReporting()
	<-reportingDone

	// The shutdown snapshot is re-sampled rather than reusing the last periodic one, which could be a
	// whole interval old. An operator reads this line to see whether the process drained its work
	// before stopping, so it has to describe the state at exit.
	shutdownCtx, cancelShutdown := context.WithTimeout(context.Background(), shutdownSampleTimeout)
	if err := collector.Collect(shutdownCtx); err != nil {
		logger.Warn("final stream sampling failed", "error", err.Error())
	}
	cancelShutdown()
	logger.Info("stream state at shutdown", "metrics", collector.Summary())
	return runErr
}

// ordersOf builds the order domain service over the same database the worker uses.
//
// It is the composition BE-I-01 exists for: the A line owns the order rules and the PostgreSQL
// store, the B line owns the worker interfaces, and the adapters in adapters.go are the only
// place the two meet. Neither side has to know about the other.
// The worker does not serve the receipt endpoint and therefore never bills, so it keeps the
// default billing timezone; the option is threaded through so a future caller can pass
// NCS_BILLING_TZ without another signature change.
func ordersOf(db *sql.DB, options ...postgres.OrderStoreOption) *order.Service {
	store, err := postgres.NewOrderStore(db, options...)
	if err != nil {
		// NewOrderStore only rejects a nil database, which the caller has already ruled out.
		panic(fmt.Sprintf("create order store: %v", err))
	}
	service, err := order.NewService(store)
	if err != nil {
		panic(fmt.Sprintf("create order service: %v", err))
	}
	return service
}

// buildRouter registers every event type the worker accepts, with the implementations that
// apply them to the domain.
func buildRouter(capabilities *redisrepo.Capabilities, store event.ConsumptionStore, orders *order.Service, gatewayURL string, logger *slog.Logger) (*worker.Router, error) {
	applier := orderChargeApplier{orders: orders, logger: logger}
	resultApplier := orderCommandResultApplier{orders: orders}
	dispatcher, err := newCommandDispatcher(gatewayURL, orders, logger)
	if err != nil {
		return nil, err
	}

	chargeHandler, err := worker.NewChargeHandler(applier, store, capabilities.Guard, worker.ChargeHandlerConfig{
		Consumer: consumerFrom(logger),
		Scope:    "charge-event",
	})
	if err != nil {
		return nil, err
	}
	commandHandler, err := worker.NewCommandRequestHandler(dispatcher, store, capabilities.Guard, worker.CommandHandlerConfig{
		Consumer: consumerFrom(logger),
		Scope:    "charger-command",
	})
	if err != nil {
		return nil, err
	}
	completionHandler, err := worker.NewCommandCompletionHandler(resultApplier, store, capabilities.Guard, worker.CommandHandlerConfig{
		Consumer: consumerFrom(logger),
		Scope:    "charger-command-result",
	})
	if err != nil {
		return nil, err
	}

	router := worker.NewRouter()
	if err := router.RegisterAll(worker.DefaultChargeEventTypes(), chargeHandler); err != nil {
		return nil, err
	}
	if err := router.Register(event.ChargerCommandRequested, commandHandler); err != nil {
		return nil, err
	}
	if err := router.Register(event.ChargerCommandCompleted, completionHandler); err != nil {
		return nil, err
	}
	return router, nil
}

// newCommandDispatcher builds the HTTP dispatcher that reaches the charger gateway and records
// the device outcome in the domain.
//
// The address is required and has no default: a deployment that forgot to configure a gateway
// must fail at startup rather than dispatch commands to whatever happens to listen on a
// development port.
func newCommandDispatcher(gatewayURL string, orders *order.Service, logger *slog.Logger) (worker.CommandDispatcher, error) {
	parsed, err := parseGatewayURL(gatewayURL)
	if err != nil {
		return nil, fmt.Errorf("NCS_CHARGER_GATEWAY_URL: %w", err)
	}
	timeout, err := durationOr(envOr("NCS_CHARGER_GATEWAY_TIMEOUT", ""), 10*time.Second)
	if err != nil {
		return nil, err
	}
	logger.Info("command dispatcher configured", "gateway", parsed.String(), "timeout", timeout.String())
	return &httpCommandDispatcher{
		client:     &http.Client{Timeout: timeout},
		baseURL:    parsed,
		completion: orders,
		logger:     logger,
	}, nil
}

// consumerFrom is a small helper so the router builder does not need the consumer threaded
// through every call; the value is recorded on every consumption row either way.
func consumerFrom(logger *slog.Logger) string {
	if value := strings.TrimSpace(os.Getenv("NCS_WORKER_CONSUMER")); value != "" {
		return value
	}
	return "worker-" + fmt.Sprint(os.Getpid())
}

// defaultStreams are the three streams the first phase consumes, taken from the frozen
// naming baseline in the migration contract.
func defaultStreams(consumer string) []worker.StreamConfig {
	return []worker.StreamConfig{
		{Stream: event.StreamOrderEvent, Group: "order-event-workers", Consumer: consumer + "-order"},
		{Stream: event.StreamChargeEvent, Group: "charge-event-workers", Consumer: consumer + "-charge"},
		{Stream: event.StreamChargerCommand, Group: "charger-command-workers", Consumer: consumer + "-command"},
	}
}

// buildStreams applies the optional "stream=group" override.
func buildStreams(consumer, override string) ([]worker.StreamConfig, error) {
	override = strings.TrimSpace(override)
	if override == "" {
		return defaultStreams(consumer), nil
	}
	streams := make([]worker.StreamConfig, 0, 4)
	for _, entry := range strings.Split(override, ",") {
		entry = strings.TrimSpace(entry)
		if entry == "" {
			continue
		}
		// stream=group or stream=group@start_id. The start position is per stream because it
		// only matters the first time a group is created: "0-0" (the default) replays the
		// history that was published before this worker first started, while "$" deliberately
		// ignores it.
		spec, startID, hasStartID := strings.Cut(strings.TrimSpace(entry), "@")
		stream, group, found := strings.Cut(spec, "=")
		stream = strings.TrimSpace(stream)
		group = strings.TrimSpace(group)
		if !found || stream == "" || group == "" {
			return nil, fmt.Errorf("invalid stream override %q, expected stream=group or stream=group@start_id", entry)
		}
		startID = strings.TrimSpace(startID)
		if hasStartID && startID == "" {
			return nil, fmt.Errorf("invalid stream override %q, the start position after %q is empty", entry, "@")
		}
		if strings.Contains(stream, "@") || strings.Contains(group, "@") {
			return nil, fmt.Errorf("invalid stream override %q, a stream or group name must not contain %q", entry, "@")
		}
		streams = append(streams, worker.StreamConfig{
			Stream:   stream,
			Group:    group,
			Consumer: consumer + "-" + sanitizeName(stream),
			StartID:  startID,
		})
	}
	if len(streams) == 0 {
		return nil, fmt.Errorf("no usable stream override in %q", override)
	}
	return streams, nil
}

func sanitizeName(value string) string {
	clean := make([]rune, 0, len(value))
	for _, r := range value {
		switch {
		case r >= 'a' && r <= 'z', r >= 'A' && r <= 'Z', r >= '0' && r <= '9', r == '-':
			clean = append(clean, r)
		default:
			clean = append(clean, '-')
		}
	}
	return string(clean)
}

func envOr(name, fallback string) string {
	if value := os.Getenv(name); value != "" {
		return value
	}
	return fallback
}

// durationOr parses a duration, returning fallback for an empty value.
func durationOr(value string, fallback time.Duration) (time.Duration, error) {
	value = strings.TrimSpace(value)
	if value == "" {
		return fallback, nil
	}
	parsed, err := time.ParseDuration(value)
	if err != nil {
		return 0, err
	}
	if parsed <= 0 {
		return 0, fmt.Errorf("duration must be greater than zero")
	}
	return parsed, nil
}
