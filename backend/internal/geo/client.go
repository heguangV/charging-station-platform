package geo

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/url"
	"strings"
	"time"
)

// The shared Tencent Map WebService client: the single exit for every
// server-side map call, so the host, the credential, the timeout and the
// envelope handling exist once.
//
// The browser uses the Tencent JavaScript API to draw a map; this client is the
// other half and never overlaps with it. It performs geocoding, place search
// and route planning, and it holds a Server Key that the browser must never
// see - which is exactly why the assistant calls it from here instead of
// letting the client call the provider directly.

// defaultBaseURL is the provider's WebService host.
const defaultBaseURL = "https://apis.map.qq.com"

// DefaultTimeout bounds one provider call.
//
// It is deliberately short. The provider sits in the path of an interactive
// question, and an answer assembled from a local estimate in three seconds is
// worth more than a perfect answer in thirty. The route planner used a slightly
// longer budget than the place search; one shared bound is easier to reason
// about and the difference was never load-bearing.
const DefaultTimeout = 3 * time.Second

// maximumResponseBytes bounds what is read from the provider. A response larger
// than this is not a route or a place list, and reading it would be the only
// thing standing between a bad answer and an exhausted process.
const maximumResponseBytes = 2 * 1024 * 1024

// ClientConfig configures the client.
type ClientConfig struct {
	// ServerKey is the Tencent Map WebService key. Without it the client is
	// unconfigured: every call reports that the provider is unavailable and the
	// caller degrades, rather than failing the request.
	ServerKey string
	// BaseURL overrides the provider host. Only tests set it.
	BaseURL string
	// Timeout overrides DefaultTimeout.
	Timeout time.Duration
	// HTTPClient overrides the default client.
	HTTPClient *http.Client
	// Logger receives refusals. It never receives the key.
	Logger *slog.Logger
}

// Client talks to the Tencent Map WebService.
type Client struct {
	baseURL   string
	serverKey string
	http      *http.Client
	logger    *slog.Logger
}

// NewClient builds a client. A missing key is not an error: it produces an
// unconfigured client, which is a supported deployment.
func NewClient(cfg ClientConfig) *Client {
	baseURL := strings.TrimSuffix(strings.TrimSpace(cfg.BaseURL), "/")
	if baseURL == "" {
		baseURL = defaultBaseURL
	}
	timeout := cfg.Timeout
	if timeout <= 0 {
		timeout = DefaultTimeout
	}
	httpClient := cfg.HTTPClient
	if httpClient == nil {
		httpClient = &http.Client{Timeout: timeout}
	}
	logger := cfg.Logger
	if logger == nil {
		logger = slog.New(slog.NewTextHandler(io.Discard, nil))
	}
	return &Client{
		baseURL:   baseURL,
		serverKey: strings.TrimSpace(cfg.ServerKey),
		http:      httpClient,
		logger:    logger,
	}
}

// Configured reports whether a Server Key is present.
func (c *Client) Configured() bool { return c != nil && c.serverKey != "" }

// envelope is the provider's response frame.
//
// The three endpoints this client speaks do not share a payload shape - the
// place search answers under `data`, the direction under `result` and the
// coordinate translation at the top level under `locations` - but they do share
// the status frame, so that is what is decoded here and the payload is handed
// on as raw JSON.
type envelope struct {
	Status    int             `json:"status"`
	Message   string          `json:"message"`
	Data      json.RawMessage `json:"data"`
	Result    json.RawMessage `json:"result"`
	Locations json.RawMessage `json:"locations"`
}

// errProviderUnavailable reports a provider that could not answer. It is the
// only kind of error the client returns, and every caller turns it into a
// degraded answer.
var errProviderUnavailable = errors.New("geo: the map service is unavailable")

// get performs one provider call and returns the decoded frame.
//
// It refuses to run at all without a key: an unauthenticated call would be
// answered with a provider error that reads like a service outage, and the
// caller would report the wrong reason.
func (c *Client) get(ctx context.Context, path string, parameters url.Values) (envelope, error) {
	if !c.Configured() {
		return envelope{}, fmt.Errorf("%w: no server key is configured", errProviderUnavailable)
	}

	endpoint := c.baseURL + path
	query := parameters.Encode()
	// The key is added last and never logged: it must not appear in a URL that
	// reaches a log line, an error message or the browser.
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint+"?"+query+"&key="+url.QueryEscape(c.serverKey), nil)
	if err != nil {
		return envelope{}, fmt.Errorf("%w: %v", errProviderUnavailable, err)
	}
	request.Header.Set("Accept", "application/json")

	response, err := c.http.Do(request)
	if err != nil {
		c.logger.Warn("geo: the map service could not be reached",
			"path", path, "error", err.Error())
		return envelope{}, fmt.Errorf("%w: %v", errProviderUnavailable, err)
	}
	defer func() { _ = response.Body.Close() }()

	body, err := io.ReadAll(io.LimitReader(response.Body, maximumResponseBytes+1))
	if err != nil {
		return envelope{}, fmt.Errorf("%w: %v", errProviderUnavailable, err)
	}
	if len(body) > maximumResponseBytes {
		c.logger.Warn("geo: the map service answered more than the client will read", "path", path)
		return envelope{}, fmt.Errorf("%w: the response was too large", errProviderUnavailable)
	}
	if response.StatusCode != http.StatusOK {
		c.logger.Warn("geo: the map service answered with a status",
			"path", path, "status", response.StatusCode)
		return envelope{}, fmt.Errorf("%w: http %d", errProviderUnavailable, response.StatusCode)
	}

	var frame envelope
	if err := json.Unmarshal(body, &frame); err != nil {
		return envelope{}, fmt.Errorf("%w: the response was not JSON", errProviderUnavailable)
	}
	if frame.Status != 0 {
		// The provider's message is a business status description and is safe to
		// put in a log line; it is never forwarded to the client.
		c.logger.Warn("geo: the map service refused the request",
			"path", path, "status", frame.Status, "message", frame.Message)
		return envelope{}, fmt.Errorf("%w: provider status %d", errProviderUnavailable, frame.Status)
	}
	return frame, nil
}
