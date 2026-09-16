package main

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/heguangV/charging-station-platform/backend/internal/admin"
	"github.com/heguangV/charging-station-platform/backend/internal/agent"
	"github.com/heguangV/charging-station-platform/backend/internal/auth"
	"github.com/heguangV/charging-station-platform/backend/internal/config"
	"github.com/heguangV/charging-station-platform/backend/internal/event"
	"github.com/heguangV/charging-station-platform/backend/internal/geo"
	"github.com/heguangV/charging-station-platform/backend/internal/httpapi"
	"github.com/heguangV/charging-station-platform/backend/internal/llm"
	"github.com/heguangV/charging-station-platform/backend/internal/observability"
	"github.com/heguangV/charging-station-platform/backend/internal/order"
	"github.com/heguangV/charging-station-platform/backend/internal/repository/postgres"
	bredis "github.com/heguangV/charging-station-platform/backend/internal/repository/redis"
	"github.com/heguangV/charging-station-platform/backend/internal/review"
	"github.com/heguangV/charging-station-platform/backend/internal/station"
	"github.com/heguangV/charging-station-platform/backend/internal/wallet"
	"github.com/heguangV/charging-station-platform/backend/migrations"
)

func main() {
	if err := run(); err != nil {
		slog.Error("api stopped with error", "error", err)
		os.Exit(1)
	}
}

func run() error {
	cfg, err := config.Load()
	if err != nil {
		return err
	}
	if cfg.PostgresDSN == "" {
		return fmt.Errorf("NCS_POSTGRES_DSN is required: the API serves business data from PostgreSQL")
	}
	// The charger gateway's receipt endpoint advances orders and starts bills.
	// Without a token it cannot be exposed at all, so the process refuses to
	// start instead of running an unauthenticated order-writing endpoint.
	if cfg.ChargerGatewayToken == "" {
		return fmt.Errorf("NCS_CHARGER_GATEWAY_TOKEN is required: the charger event endpoint advances orders and billing")
	}

	logger := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelInfo}))

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	// PostgreSQL is the source of truth: connect and migrate before binding
	// the port so a broken database fails startup instead of serving errors.
	// Migrations run on a single pooled connection, so small pools are safe.
	db, err := postgres.Open(ctx, cfg.PostgresDSN, cfg.PostgresMaxConns)
	if err != nil {
		return err
	}
	defer db.Close()

	migrationDB, err := postgres.NewSQLDB(db)
	if err != nil {
		return err
	}
	report, err := postgres.Run(ctx, migrationDB, migrations.FS)
	if err != nil {
		return fmt.Errorf("apply migrations: %w", err)
	}
	if len(report.Applied) > 0 {
		logger.Info("database migrations applied", "count", len(report.Applied))
	}
	if migrateOnlyRequested() {
		return nil
	}

	// Redis carries sessions, login rate limiting and SMS codes. Sessions
	// must be shared so any replica validates any token; Redis is therefore
	// a hard startup dependency (settings via NCS_REDIS_*).
	redisConfig, err := bredis.ConnConfigFromEnv(nil)
	if err != nil {
		return fmt.Errorf("redis configuration: %w", err)
	}
	redisClient, err := bredis.NewClient(redisConfig)
	if err != nil {
		return err
	}
	defer redisClient.Close()
	commands, err := redisClient.Open(ctx, redisConfig)
	if err != nil {
		return fmt.Errorf("open redis: %w", err)
	}
	if err := commands.Ping(ctx); err != nil {
		return fmt.Errorf("redis ping: %w (sessions, login limiting and SMS codes require Redis; configure NCS_REDIS_*)", err)
	}

	degradation := bredis.NewDegradation()
	policy := bredis.DefaultPolicy()
	redisSessions, err := bredis.NewSessions(commands, policy, degradation, bredis.SessionConfig{
		IdleTTL:     cfg.SessionIdleTTL,
		AbsoluteTTL: cfg.SessionAbsTTL,
	}, nil)
	if err != nil {
		return err
	}
	redisLimiter, err := bredis.NewLimiter(commands, policy, degradation, nil)
	if err != nil {
		return err
	}

	accountStore, err := postgres.NewAccountStore(db)
	if err != nil {
		return err
	}
	stationStore, err := postgres.NewStationStore(db)
	if err != nil {
		return err
	}
	// The billing timezone decides which wall-clock hours the off-peak tariff window means.
	orderStore, err := postgres.NewOrderStore(db,
		postgres.WithBillingLocation(cfg.BillingLocation),
		postgres.WithReservationDuration(cfg.OrderExpireAfter),
	)
	if err != nil {
		return err
	}

	// Shared-state session storage and login throttling (A-03 review: every
	// API instance must accept every token).
	sessionStore, err := auth.NewRedisSessionStore(redisSessions, commands)
	if err != nil {
		return err
	}
	loginLimiter, err := auth.NewRedisLoginRateLimiter(redisLimiter, cfg.LoginRateLimit, cfg.LoginRateWindow)
	if err != nil {
		return err
	}
	smsCodes, err := auth.NewRedisSMSCodeStore(commands)
	if err != nil {
		return err
	}

	mutationAdapter, err := postgres.NewAccountMutationAdapter(db)
	if err != nil {
		return err
	}
	authService, err := auth.NewService(accountStore, accountStore, mutationAdapter, sessionStore, loginLimiter, smsCodes, cfg.SessionIdleTTL, cfg.SessionAbsTTL)
	if err != nil {
		return err
	}
	authService.SMSMock = cfg.SMSMock
	if cfg.SMSSenderURL != "" {
		sender, err := auth.NewHTTPSMSSender(auth.SenderConfig{URL: cfg.SMSSenderURL, Token: cfg.SMSSenderToken})
		if err != nil {
			return err
		}
		authService.Sender = sender
		logger.Info("sms delivery through http gateway", "url", cfg.SMSSenderURL)
	} else if !cfg.SMSMock {
		logger.Warn("no sms sender configured: code requests will fail until NCS_SMS_SENDER_URL is set")
	}
	stationService, err := station.NewService(stationStore)
	if err != nil {
		return err
	}
	orderService, err := order.NewService(orderStore)
	if err != nil {
		return err
	}
	adminStore, err := postgres.NewAdminStore(db)
	if err != nil {
		return err
	}
	adminService, err := admin.NewService(adminStore)
	if err != nil {
		return err
	}

	authHandlers, err := auth.NewHandlers(authService)
	if err != nil {
		return err
	}
	stationHandlers, err := station.NewHandlers(stationService, authHandlers)
	if err != nil {
		return err
	}
	orderHandlers, err := order.NewHandlers(orderService, authHandlers)
	if err != nil {
		return err
	}
	adminHandlers, err := admin.NewHandlers(adminService, authHandlers)
	if err != nil {
		return err
	}
	// The charger gateway reports the physical facts of a charge here: a start
	// receipt moves an order to CHARGING, a stop receipt completes it and
	// releases the charger (BE-I-02).
	chargerEventHandlers, err := order.NewChargerEventHandlers(orderService, order.ChargerEventConfig{
		Token:         cfg.ChargerGatewayToken,
		MaxFutureSkew: cfg.FactTimeSkew,
		Logger:        logger,
	})
	if err != nil {
		return err
	}

	server := httpapi.NewServer(cfg, logger)
	registry := observability.NewRegistry()
	registry.RegisterHistogram(observability.MetricRequestDuration, observability.DefaultDurationBuckets)
	observability.RegisterProcessMetrics(registry, observability.ProcessMetricsConfig{
		Streams: []string{event.StreamOrderEvent, event.StreamChargeEvent, event.StreamChargerCommand},
	})
	instrumented := registerWithMetrics{inner: server, registry: registry}
	authHandlers.Register(instrumented)
	stationHandlers.Register(instrumented)
	orderHandlers.Register(instrumented)
	adminHandlers.Register(instrumented)

	walletStore, err := postgres.NewWalletStore(db)
	if err != nil {
		return err
	}
	walletService, err := wallet.NewService(walletStore)
	if err != nil {
		return err
	}
	walletHandlers, err := wallet.NewHandlers(walletService, authHandlers)
	if err != nil {
		return err
	}
	walletHandlers.Register(instrumented)

	reviewStore, err := postgres.NewReviewStore(db)
	if err != nil {
		return err
	}
	reviewService, err := review.NewService(reviewStore)
	if err != nil {
		return err
	}
	reviewHandlers, err := review.NewHandlers(reviewService, authHandlers, authHandlers)
	if err != nil {
		return err
	}
	reviewHandlers.Register(instrumented)
	chargerEventHandlers.Register(instrumented)

	// Agent is read-only. Map and model providers are optional; without them
	// the service still returns deterministic station answers with degraded=true.
	summaryService, err := station.NewSummaryService(stationStore)
	if err != nil {
		return err
	}
	mapClient := geo.NewClient(geo.ClientConfig{
		ServerKey: cfg.Assistant.MapServerKey,
		BaseURL:   cfg.Assistant.MapBaseURL,
		Logger:    logger,
	})
	modelClient, err := llm.New(llm.Config{
		Provider: cfg.Assistant.LLMProvider,
		Model:    cfg.Assistant.LLMModel,
		BaseURL:  cfg.Assistant.LLMBaseURL,
		APIKey:   cfg.Assistant.LLMAPIKey,
		Timeout:  cfg.Assistant.LLMTimeout,
		Logger:   logger,
	})
	if err != nil {
		return err
	}
	assistantService, err := agent.NewService(agent.Config{
		Stations:  summaryService,
		Pois:      mapClient,
		Routes:    mapClient,
		LLM:       modelClient,
		Converter: mapClient,
		Logger:    logger,
	})
	if err != nil {
		return err
	}
	assistantHandlers, err := agent.NewHandlers(assistantService, authHandlers)
	if err != nil {
		return err
	}
	logger.Info("assistant capability", "map_provider", mapClient.Configured(), "model", modelClient.Available(), "tools", assistantService.ToolNames())
	assistantHandlers.Register(instrumented)

	metrics, err := metricsServer(metricsConfig{
		envName:        metricsAddrEnv,
		allowPublicEnv: metricsAllowPublicEnv,
		fallback:       defaultMetricsAddr,
	}, registry, logger)
	if err != nil {
		return err
	}
	defer func() {
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		if err := metrics.Shutdown(shutdownCtx); err != nil {
			logger.Error("shutdown metrics endpoint", "error", err)
		}
	}()
	probeCtx, stopProbe := context.WithCancel(ctx)
	probeDone := make(chan struct{})
	go func() {
		defer close(probeDone)
		observability.ProbeDependencies(probeCtx, observability.ProbeConfig{
			PostgresUp:      func(ctx context.Context) bool { return db.PingContext(ctx) == nil },
			RedisUp:         func(ctx context.Context) bool { return commands.Ping(ctx) == nil },
			SchemaVersion:   func(ctx context.Context) (int, error) { return postgres.SchemaVersion(ctx, db) },
			OutboxBacklog:   func(ctx context.Context) (int64, error) { return postgres.OutboxBacklog(ctx, db) },
			OutboxOldestAge: func(ctx context.Context) (float64, error) { return postgres.OldestUnpublishedOutboxAge(ctx, db) },
			Registry:        registry, Logger: logger, Interval: dependencyProbeInterval, Ready: server.SetReady,
		})
	}()
	defer func() { stopProbe(); <-probeDone }()

	listener, err := net.Listen("tcp", cfg.HTTPAddr)
	if err != nil {
		return fmt.Errorf("listen on %s: %w", cfg.HTTPAddr, err)
	}
	defer listener.Close()

	// Janitor: expire abandoned CREATED orders (EXPIRED) and stuck STARTING
	// orders (FAILED), releasing their chargers so an unstarted order cannot
	// occupy a device forever.
	janitorStop := make(chan struct{})
	janitorDone := make(chan struct{})
	go func() {
		defer close(janitorDone)
		ticker := time.NewTicker(time.Minute)
		defer ticker.Stop()
		for {
			select {
			case <-janitorStop:
				return
			case <-ticker.C:
				released, err := orderService.ExpireStaleOrders(ctx, cfg.OrderExpireAfter)
				if err != nil {
					logger.Error("order expiration sweep failed", "error", err)
				} else if released > 0 {
					logger.Info("stale orders released", "count", released)
				}
				// Chargers held by terminal orders are freed only after the
				// revocation command was queued; this sweep performs it.
				freed, err := orderService.ReleaseOrphanedChargers(ctx)
				if err != nil {
					logger.Error("orphaned charger sweep failed", "error", err)
				} else if freed > 0 {
					logger.Info("orphaned chargers released", "count", freed)
				}
				// Orders stuck in STOPPING with a faulty charger (BE-I-02): the device refused
				// a stop, so the platform re-sends STOP_CHARGING with a new command id a
				// bounded number of times. It never releases the charger and never settles the
				// order - the device may still be charging - and after the last attempt the
				// order waits for an operator instead.
				recovery, err := orderService.ReissueStopCommands(ctx, order.StopRecoveryPolicy{
					MaxAttempts: cfg.StopRecoveryAttempts,
					Backoff:     cfg.StopRecoveryBackoff,
				})
				if err != nil {
					logger.Error("stop recovery sweep failed", "error", err)
				} else {
					if recovery.Reissued > 0 {
						logger.Info("stop commands reissued", "count", recovery.Reissued, "waiting", recovery.Waiting)
					}
					if recovery.Escalated > 0 {
						logger.Error("orders need manual intervention after repeated stop failures",
							"count", recovery.Escalated, "attempt_limit", cfg.StopRecoveryAttempts,
							"order_status", "STOPPING", "charger_status", "FAULT")
					}
				}
			}
		}
	}()
	defer func() {
		close(janitorStop)
		<-janitorDone
	}()

	server.SetReady(true)
	serverErr := make(chan error, 1)
	go func() {
		serverErr <- server.Serve(listener)
	}()

	signals := make(chan os.Signal, 1)
	signal.Notify(signals, syscall.SIGINT, syscall.SIGTERM)
	defer signal.Stop(signals)

	select {
	case err := <-serverErr:
		if errors.Is(err, http.ErrServerClosed) {
			return nil
		}
		return err
	case sig := <-signals:
		logger.Info("shutdown signal received", "signal", sig.String())
		shutdownContext, cancel := context.WithTimeout(context.Background(), cfg.ShutdownTimeout)
		defer cancel()
		if err := server.Shutdown(shutdownContext); err != nil {
			return fmt.Errorf("graceful shutdown: %w", err)
		}
		return nil
	}
}
