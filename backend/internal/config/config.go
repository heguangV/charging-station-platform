package config

import (
	"fmt"
	"os"
	"strconv"
	"strings"
	"time"
)

const (
	defaultHTTPAddr         = ":8080"
	defaultEnvironment      = "development"
	defaultRequestIDHeader  = "X-Request-ID"
	defaultShutdownTimeout  = 10 * time.Second
	defaultPostgresMaxConns = 10
	defaultSessionIdleTTL   = 30 * time.Minute
	defaultSessionAbsTTL    = 168 * time.Hour // requirements: sessions live at least seven days
	defaultLoginRateLimit   = 10
	defaultLoginRateWindow  = time.Minute
	envHTTPAddr             = "NCS_HTTP_ADDR"
	envEnvironment          = "NCS_ENV"
	envRequestIDHeader      = "NCS_REQUEST_ID_HEADER"
	envShutdownTimeout      = "NCS_SHUTDOWN_TIMEOUT"
	envPostgresDSN          = "NCS_POSTGRES_DSN"
	envPostgresMaxConns     = "NCS_POSTGRES_MAX_CONNS"
	envSessionIdleTTL       = "NCS_SESSION_IDLE_TTL"
	envSessionAbsTTL        = "NCS_SESSION_ABSOLUTE_TTL"
	envLoginRateLimit       = "NCS_LOGIN_RATE_LIMIT"
	envLoginRateWindow      = "NCS_LOGIN_RATE_WINDOW"
	envSMSMock              = "NCS_SMS_MOCK"
	envSMSSenderURL         = "NCS_SMS_SENDER_URL"
	envSMSSenderToken       = "NCS_SMS_SENDER_TOKEN"
	envOrderExpireAfter     = "NCS_ORDER_EXPIRE_AFTER"
	envBillingTimezone      = "NCS_BILLING_TZ"
	envChargerGatewayToken  = "NCS_CHARGER_GATEWAY_TOKEN"
	envFactTimeSkew         = "NCS_CHARGER_EVENT_MAX_FUTURE_SKEW"
	envStopRecoveryAttempts = "NCS_STOP_RECOVERY_MAX_ATTEMPTS"
	envStopRecoveryBackoff  = "NCS_STOP_RECOVERY_BACKOFF"
	envMapServerKey         = "TENCENT_MAP_SERVER_KEY"
	envMapBaseURL           = "TENCENT_MAP_BASE_URL"
	envLLMProvider          = "AI_PROVIDER"
	envLLMModel             = "AI_MODEL"
	envLLMBaseURL           = "AI_BASE_URL"
	envLLMAPIKey            = "AI_API_KEY"
	envLLMTimeoutMS         = "AI_TIMEOUT_MS"
	sessionMinIdleTTL       = time.Second
	sessionMinAbsoluteTTL   = 168 * time.Hour // A-03 review: the absolute session window may not be shortened below seven days
	loginMinRateLimit       = 1
	loginMinRateWindow      = time.Second
	defaultOrderExpireAfter = 15 * time.Minute // UC-U-07: 15-minute reservation window
	// defaultBillingTimezone is the wall-clock timezone whose hours define the
	// off-peak tariff window. The fleet bills Chinese operators, whose off-peak
	// window is local night (23:00-07:00); pricing those hours against UTC put the
	// discount in the middle of the local day.
	defaultBillingTimezone = "Asia/Shanghai"
	// BE-I-02 receipt and STOP recovery bounds. The skew is the frozen 5-minute
	// default; the recovery limit is deliberately conservative, because every
	// re-send talks to a device that has already refused one command.
	defaultFactTimeSkew         = 5 * time.Minute
	defaultStopRecoveryAttempts = 3
	defaultStopRecoveryBackoff  = 5 * time.Minute
	defaultLLMTimeoutMS         = 15000
)

type AssistantConfig struct {
	MapServerKey string
	MapBaseURL   string
	LLMProvider  string
	LLMModel     string
	LLMBaseURL   string
	LLMAPIKey    string
	LLMTimeout   time.Duration
}

// Config contains process-level settings for the API service.
//
// PostgresDSN is empty only in the A-01 bootstrap state; the API process
// refuses to start business features without it. Redis settings are read by
// the B-line redis.ConnConfigFromEnv (NCS_REDIS_*).
type Config struct {
	HTTPAddr         string
	Environment      string
	RequestIDHeader  string
	ShutdownTimeout  time.Duration
	PostgresDSN      string
	PostgresMaxConns int
	SessionIdleTTL   time.Duration
	SessionAbsTTL    time.Duration
	LoginRateLimit   int
	LoginRateWindow  time.Duration
	// SMSMock returns generated login codes in the API response instead of
	// delivering them through an SMS provider. It defaults to true in the
	// development environment and must be disabled in formal deployments.
	SMSMock bool
	// OrderExpireAfter bounds how long an unstarted CREATED order may hold
	// its charger before the janitor expires it.
	OrderExpireAfter time.Duration
	// BillingLocation is the wall-clock timezone the off-peak tariff window is
	// expressed in (NCS_BILLING_TZ, default Asia/Shanghai). Billing itself stays
	// in UTC instants; only the window lookup uses these wall-clock hours.
	BillingLocation *time.Location
	// SMSSenderURL and SMSSenderToken configure the HTTP SMS gateway used
	// when simulated delivery is off (see auth.SMSSender).
	SMSSenderURL   string
	SMSSenderToken string
	// ChargerGatewayToken authenticates the charger gateway on the internal
	// receipt endpoint (POST /api/v1/internal/charger-events). It has no
	// default: a receipt advances an order and starts a bill, so the API
	// refuses to start without it.
	ChargerGatewayToken string
	// FactTimeSkew bounds how far a device-reported fact time may run ahead of
	// the server clock before the receipt is rejected.
	FactTimeSkew time.Duration
	// StopRecoveryAttempts and StopRecoveryBackoff bound the STOP recovery
	// sweep: how many STOP_CHARGING commands one order may accumulate in total,
	// and how long the sweep waits between two of them.
	StopRecoveryAttempts int
	StopRecoveryBackoff  time.Duration
	Assistant            AssistantConfig
}

// Load reads configuration from the process environment and applies safe
// development defaults. Empty values are treated as unset.
func Load() (Config, error) {
	timeout, err := durationFromEnv(envShutdownTimeout, defaultShutdownTimeout)
	if err != nil {
		return Config{}, err
	}
	idleTTL, err := durationFromEnv(envSessionIdleTTL, defaultSessionIdleTTL)
	if err != nil {
		return Config{}, err
	}
	absoluteTTL, err := durationFromEnv(envSessionAbsTTL, defaultSessionAbsTTL)
	if err != nil {
		return Config{}, err
	}
	rateWindow, err := durationFromEnv(envLoginRateWindow, defaultLoginRateWindow)
	if err != nil {
		return Config{}, err
	}
	maxConns, err := intFromEnv(envPostgresMaxConns, defaultPostgresMaxConns)
	if err != nil {
		return Config{}, err
	}
	rateLimit, err := intFromEnv(envLoginRateLimit, defaultLoginRateLimit)
	if err != nil {
		return Config{}, err
	}
	expireAfter, err := durationFromEnv(envOrderExpireAfter, defaultOrderExpireAfter)
	if err != nil {
		return Config{}, err
	}
	billingLocation, err := locationFromEnv(envBillingTimezone, defaultBillingTimezone)
	if err != nil {
		return Config{}, err
	}
	factTimeSkew, err := durationFromEnv(envFactTimeSkew, defaultFactTimeSkew)
	if err != nil {
		return Config{}, err
	}
	stopRecoveryBackoff, err := durationFromEnv(envStopRecoveryBackoff, defaultStopRecoveryBackoff)
	if err != nil {
		return Config{}, err
	}
	stopRecoveryAttempts, err := intFromEnv(envStopRecoveryAttempts, defaultStopRecoveryAttempts)
	if err != nil {
		return Config{}, err
	}
	llmTimeoutMS, err := intFromEnv(envLLMTimeoutMS, defaultLLMTimeoutMS)
	if err != nil {
		return Config{}, err
	}
	if llmTimeoutMS < 1 {
		return Config{}, fmt.Errorf("invalid %s=%d: must be at least 1", envLLMTimeoutMS, llmTimeoutMS)
	}
	if stopRecoveryAttempts < 1 {
		return Config{}, fmt.Errorf("invalid %s=%d: must be at least 1", envStopRecoveryAttempts, stopRecoveryAttempts)
	}
	if expireAfter < time.Minute {
		return Config{}, fmt.Errorf("invalid %s=%q: must be at least 1m", envOrderExpireAfter, expireAfter)
	}

	if idleTTL < sessionMinIdleTTL {
		return Config{}, fmt.Errorf("invalid %s=%q: must be at least %s", envSessionIdleTTL, idleTTL, sessionMinIdleTTL)
	}
	if absoluteTTL < sessionMinAbsoluteTTL {
		return Config{}, fmt.Errorf("invalid %s=%q: must be at least %s", envSessionAbsTTL, absoluteTTL, sessionMinAbsoluteTTL)
	}
	if absoluteTTL < idleTTL {
		return Config{}, fmt.Errorf("invalid %s: absolute TTL (%s) must not be shorter than idle TTL (%s)", envSessionAbsTTL, absoluteTTL, idleTTL)
	}
	if rateLimit < loginMinRateLimit {
		return Config{}, fmt.Errorf("invalid %s=%d: must be at least %d", envLoginRateLimit, rateLimit, loginMinRateLimit)
	}
	if rateWindow < loginMinRateWindow {
		return Config{}, fmt.Errorf("invalid %s=%q: must be at least %s", envLoginRateWindow, rateWindow, loginMinRateWindow)
	}

	environment := valueOrDefault(envEnvironment, defaultEnvironment)
	// Simulated SMS must never reach a formal deployment, no matter what the
	// environment variables say (AGENTS: 正式环境禁用模拟短信).
	smsMock := smsMockFromEnv(environment)
	if smsMock && environment != "development" {
		return Config{}, fmt.Errorf("invalid %s=true: simulated SMS is only allowed in the development environment", envSMSMock)
	}
	return Config{
		HTTPAddr:         valueOrDefault(envHTTPAddr, defaultHTTPAddr),
		Environment:      environment,
		RequestIDHeader:  valueOrDefault(envRequestIDHeader, defaultRequestIDHeader),
		ShutdownTimeout:  timeout,
		PostgresDSN:      strings.TrimSpace(os.Getenv(envPostgresDSN)),
		PostgresMaxConns: maxConns,
		SessionIdleTTL:   idleTTL,
		SessionAbsTTL:    absoluteTTL,
		LoginRateLimit:   rateLimit,
		LoginRateWindow:  rateWindow,
		SMSMock:          smsMockFromEnv(environment),
		OrderExpireAfter: expireAfter,
		BillingLocation:  billingLocation,
		SMSSenderURL:     strings.TrimSpace(os.Getenv(envSMSSenderURL)),
		SMSSenderToken:   strings.TrimSpace(os.Getenv(envSMSSenderToken)),
		// No default: an empty token keeps the receipt endpoint closed and the
		// API refuses to start (see cmd/api), rather than exposing an endpoint
		// that can advance orders.
		ChargerGatewayToken:  strings.TrimSpace(os.Getenv(envChargerGatewayToken)),
		FactTimeSkew:         factTimeSkew,
		StopRecoveryAttempts: stopRecoveryAttempts,
		StopRecoveryBackoff:  stopRecoveryBackoff,
		Assistant: AssistantConfig{
			MapServerKey: strings.TrimSpace(os.Getenv(envMapServerKey)),
			MapBaseURL:   strings.TrimSpace(os.Getenv(envMapBaseURL)),
			LLMProvider:  strings.TrimSpace(os.Getenv(envLLMProvider)),
			LLMModel:     strings.TrimSpace(os.Getenv(envLLMModel)),
			LLMBaseURL:   strings.TrimSpace(os.Getenv(envLLMBaseURL)),
			LLMAPIKey:    strings.TrimSpace(os.Getenv(envLLMAPIKey)),
			LLMTimeout:   time.Duration(llmTimeoutMS) * time.Millisecond,
		},
	}, nil
}

// smsMockFromEnv defaults simulated SMS to the development environment; an
// explicit NCS_SMS_MOCK always wins.
func smsMockFromEnv(environment string) bool {
	if value := strings.TrimSpace(os.Getenv(envSMSMock)); value != "" {
		return value == "true" || value == "1"
	}
	return environment == "development"
}

func valueOrDefault(name, fallback string) string {
	if value := strings.TrimSpace(os.Getenv(name)); value != "" {
		return value
	}
	return fallback
}

// locationFromEnv resolves a timezone name (IANA, e.g. Asia/Shanghai) into a
// location. A missing value keeps the product default; a name the runtime cannot
// resolve is a hard error, because silently falling back to UTC would move the
// off-peak window by the deployment's offset.
func locationFromEnv(name string, fallback string) (*time.Location, error) {
	value := strings.TrimSpace(os.Getenv(name))
	if value == "" {
		value = fallback
	}
	location, err := time.LoadLocation(value)
	if err != nil {
		return nil, fmt.Errorf("invalid %s=%q: %w", name, value, err)
	}
	return location, nil
}

func durationFromEnv(name string, fallback time.Duration) (time.Duration, error) {
	value := strings.TrimSpace(os.Getenv(name))
	if value == "" {
		return fallback, nil
	}

	duration, err := time.ParseDuration(value)
	if err != nil || duration <= 0 {
		if err == nil {
			err = fmt.Errorf("duration must be greater than zero")
		}
		return 0, fmt.Errorf("invalid %s=%q: %w", name, value, err)
	}
	return duration, nil
}

func intFromEnv(name string, fallback int) (int, error) {
	value := strings.TrimSpace(os.Getenv(name))
	if value == "" {
		return fallback, nil
	}

	parsed, err := strconv.Atoi(value)
	if err != nil {
		return 0, fmt.Errorf("invalid %s=%q: must be an integer", name, value)
	}
	return parsed, nil
}
