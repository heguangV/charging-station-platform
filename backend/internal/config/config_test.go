package config

import (
	"testing"
	"time"
)

func TestLoadUsesDefaults(t *testing.T) {
	for _, name := range []string{
		envHTTPAddr, envEnvironment, envRequestIDHeader, envShutdownTimeout,
		envPostgresDSN, envPostgresMaxConns, envSessionIdleTTL, envSessionAbsTTL,
		envLoginRateLimit, envLoginRateWindow,
	} {
		t.Setenv(name, "")
	}

	got, err := Load()
	if err != nil {
		t.Fatalf("Load() error = %v", err)
	}

	if got.HTTPAddr != defaultHTTPAddr {
		t.Fatalf("HTTPAddr = %q, want %q", got.HTTPAddr, defaultHTTPAddr)
	}
	if got.Environment != defaultEnvironment {
		t.Fatalf("Environment = %q, want %q", got.Environment, defaultEnvironment)
	}
	if got.RequestIDHeader != defaultRequestIDHeader {
		t.Fatalf("RequestIDHeader = %q, want %q", got.RequestIDHeader, defaultRequestIDHeader)
	}
	if got.ShutdownTimeout != defaultShutdownTimeout {
		t.Fatalf("ShutdownTimeout = %s, want %s", got.ShutdownTimeout, defaultShutdownTimeout)
	}
	if got.PostgresDSN != "" {
		t.Fatalf("PostgresDSN = %q, want empty", got.PostgresDSN)
	}
	if got.PostgresMaxConns != defaultPostgresMaxConns {
		t.Fatalf("PostgresMaxConns = %d, want %d", got.PostgresMaxConns, defaultPostgresMaxConns)
	}
	if got.SessionIdleTTL != defaultSessionIdleTTL {
		t.Fatalf("SessionIdleTTL = %s, want %s", got.SessionIdleTTL, defaultSessionIdleTTL)
	}
	if got.SessionAbsTTL != defaultSessionAbsTTL {
		t.Fatalf("SessionAbsTTL = %s, want %s", got.SessionAbsTTL, defaultSessionAbsTTL)
	}
	if got.LoginRateLimit != defaultLoginRateLimit {
		t.Fatalf("LoginRateLimit = %d, want %d", got.LoginRateLimit, defaultLoginRateLimit)
	}
	if got.LoginRateWindow != defaultLoginRateWindow {
		t.Fatalf("LoginRateWindow = %s, want %s", got.LoginRateWindow, defaultLoginRateWindow)
	}
}

func TestLoadReadsEnvironment(t *testing.T) {
	t.Setenv(envHTTPAddr, "127.0.0.1:18080")
	t.Setenv(envEnvironment, "test")
	t.Setenv(envRequestIDHeader, "X-Correlation-ID")
	t.Setenv(envShutdownTimeout, "250ms")
	t.Setenv(envPostgresDSN, "postgres://ncs:secret@127.0.0.1:5432/ncs_test")
	t.Setenv(envPostgresMaxConns, "4")
	t.Setenv(envSessionIdleTTL, "5m")
	t.Setenv(envSessionAbsTTL, "240h")
	t.Setenv(envLoginRateLimit, "3")
	t.Setenv(envLoginRateWindow, "30s")

	got, err := Load()
	if err != nil {
		t.Fatalf("Load() error = %v", err)
	}

	if got.HTTPAddr != "127.0.0.1:18080" || got.Environment != "test" {
		t.Fatalf("environment values = %#v", got)
	}
	if got.RequestIDHeader != "X-Correlation-ID" {
		t.Fatalf("RequestIDHeader = %q", got.RequestIDHeader)
	}
	if got.ShutdownTimeout != 250*time.Millisecond {
		t.Fatalf("ShutdownTimeout = %s", got.ShutdownTimeout)
	}
	if got.PostgresDSN != "postgres://ncs:secret@127.0.0.1:5432/ncs_test" {
		t.Fatalf("PostgresDSN = %q", got.PostgresDSN)
	}
	if got.PostgresMaxConns != 4 {
		t.Fatalf("PostgresMaxConns = %d", got.PostgresMaxConns)
	}
	if got.SessionIdleTTL != 5*time.Minute || got.SessionAbsTTL != 240*time.Hour {
		t.Fatalf("session TTLs = %s/%s", got.SessionIdleTTL, got.SessionAbsTTL)
	}
	if got.LoginRateLimit != 3 || got.LoginRateWindow != 30*time.Second {
		t.Fatalf("login rate = %d/%s", got.LoginRateLimit, got.LoginRateWindow)
	}
}

func TestLoadRejectsInvalidShutdownTimeout(t *testing.T) {
	t.Setenv(envShutdownTimeout, "0s")

	if _, err := Load(); err == nil {
		t.Fatal("Load() error = nil, want invalid duration error")
	}
}

func TestLoadRejectsInvalidSessionTTLs(t *testing.T) {
	t.Setenv(envSessionIdleTTL, "0s")
	if _, err := Load(); err == nil {
		t.Fatal("zero idle TTL accepted")
	}

	t.Setenv(envSessionIdleTTL, "240h")
	t.Setenv(envSessionAbsTTL, "200h")
	if _, err := Load(); err == nil {
		t.Fatal("absolute TTL shorter than idle TTL accepted")
	}
}

func TestLoadRejectsInvalidRateSettings(t *testing.T) {
	t.Setenv(envLoginRateLimit, "0")
	if _, err := Load(); err == nil {
		t.Fatal("zero rate limit accepted")
	}

	t.Setenv(envLoginRateLimit, "5")
	t.Setenv(envLoginRateWindow, "0s")
	if _, err := Load(); err == nil {
		t.Fatal("zero rate window accepted")
	}
}

func TestLoadRejectsInvalidPostgresMaxConns(t *testing.T) {
	t.Setenv(envPostgresMaxConns, "many")

	if _, err := Load(); err == nil {
		t.Fatal("non-integer max conns accepted")
	}
}

func TestLoadRejectsSMSMockOutsideDevelopment(t *testing.T) {
	t.Setenv(envEnvironment, "production")
	t.Setenv(envSMSMock, "true")

	if _, err := Load(); err == nil {
		t.Fatal("simulated SMS accepted outside the development environment")
	}

	t.Setenv(envSMSMock, "false")
	got, err := Load()
	if err != nil {
		t.Fatalf("Load() error = %v", err)
	}
	if got.SMSMock {
		t.Fatal("mock SMS enabled with an explicit false")
	}
}

func TestLoadRejectsMapBaseURLThatWouldLeakTheServerKey(t *testing.T) {
	for _, value := range []string{
		"http://apis.map.qq.com",
		"http://localhost.attacker.example",
		"ftp://apis.map.qq.com",
		"apis.map.qq.com",
	} {
		t.Setenv(envMapBaseURL, value)
		if _, err := Load(); err == nil {
			t.Errorf("%s=%q accepted; the Server Key would be sent to it", envMapBaseURL, value)
		}
	}
}

func TestLoadAcceptsMapBaseURLOverHTTPSOrLoopback(t *testing.T) {
	for _, value := range []string{
		"",
		"https://apis.map.qq.com",
		"http://127.0.0.1:18090",
		"http://localhost:18090/",
		"http://[::1]:18090",
	} {
		t.Setenv(envMapBaseURL, value)
		cfg, err := Load()
		if err != nil {
			t.Fatalf("%s=%q rejected: %v", envMapBaseURL, value, err)
		}
		if cfg.Assistant.MapBaseURL != value {
			t.Fatalf("MapBaseURL = %q, want %q", cfg.Assistant.MapBaseURL, value)
		}
	}
}
