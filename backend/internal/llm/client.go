// Package llm adapts an OpenAI-compatible chat endpoint to the assistant's
// model port.
//
// The model is optional by design. A deployment with no credential is fully
// supported and produces the deterministic answers; a deployment with one gets
// better wording and a model that can choose which tools to run. The two are
// the same feature at different capability levels, not a working feature and a
// broken one - which is why an unconfigured client is not an error.
package llm

import (
	"bytes"
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

	"github.com/heguangV/charging-station-platform/backend/internal/agent"
)

// The endpoint suffixes and bounds.
const (
	completionsPath = "/chat/completions"

	// minimumTimeout and maximumTimeout bound one call. The upper bound exists so
	// a misconfigured timeout cannot hold a request open indefinitely: the
	// assistant answers without the model rather than making the user wait.
	minimumTimeout = 1 * time.Second
	maximumTimeout = 60 * time.Second

	// defaultTimeout is used when nothing is configured.
	defaultTimeout = 15 * time.Second

	// maximumResponseBytes bounds a completion. A reply is prose and a few tool
	// calls; anything larger is not an answer.
	maximumResponseBytes = 1 * 1024 * 1024
)

// providerBaseURLs are the endpoint presets. A provider outside this list must
// name its endpoint explicitly.
var providerBaseURLs = map[string]string{
	"openai":    "https://api.openai.com/v1",
	"deepseek":  "https://api.deepseek.com/v1",
	"qwen":      "https://dashscope.aliyuncs.com/compatible-mode/v1",
	"dashscope": "https://dashscope.aliyuncs.com/compatible-mode/v1",
	"claude":    "https://api.anthropic.com/v1",
	"anthropic": "https://api.anthropic.com/v1",
}

// SupportedProviders lists the provider names the configuration accepts.
func SupportedProviders() []string {
	return []string{"openai", "deepseek", "qwen", "claude", "custom"}
}

// Config configures the client.
type Config struct {
	// Provider selects an endpoint preset. Empty means openai.
	Provider string
	// Model is the model to ask for. Without it the client is unconfigured.
	Model string
	// BaseURL overrides the preset. A provider with no preset must set it.
	BaseURL string
	// APIKey is the credential. Without it the client is unconfigured. It never
	// appears in a log line or an error.
	APIKey string
	// Timeout bounds one call and is clamped to a usable range.
	Timeout time.Duration
	// HTTPClient overrides the default client. Only tests set it.
	HTTPClient *http.Client
	// Logger receives provider failures. It never receives the key.
	Logger *slog.Logger
}

// Client talks to an OpenAI-compatible chat endpoint.
type Client struct {
	baseURL string
	model   string
	apiKey  string
	timeout time.Duration
	http    *http.Client
	logger  *slog.Logger
}

// New builds the client.
//
// A missing model or key is not an error: it produces a client that reports
// itself unavailable, which is the supported "no model" deployment. A model and
// key with an endpoint that cannot be used is an error, because that is a
// misconfiguration and silently degrading would hide it.
func New(cfg Config) (*Client, error) {
	model := strings.TrimSpace(cfg.Model)
	key := strings.TrimSpace(cfg.APIKey)
	logger := cfg.Logger
	if logger == nil {
		logger = slog.New(slog.NewTextHandler(io.Discard, nil))
	}
	if model == "" || key == "" {
		return &Client{logger: logger}, nil
	}

	provider := strings.ToLower(strings.TrimSpace(cfg.Provider))
	if provider == "" {
		provider = "openai"
	}
	baseURL, err := resolveBaseURL(provider, cfg.BaseURL)
	if err != nil {
		return nil, err
	}

	timeout := cfg.Timeout
	if timeout <= 0 {
		timeout = defaultTimeout
	}
	if timeout < minimumTimeout {
		timeout = minimumTimeout
	}
	if timeout > maximumTimeout {
		timeout = maximumTimeout
	}
	httpClient := cfg.HTTPClient
	if httpClient == nil {
		httpClient = &http.Client{Timeout: timeout}
	}
	return &Client{
		baseURL: baseURL,
		model:   model,
		apiKey:  key,
		timeout: timeout,
		http:    httpClient,
		logger:  logger,
	}, nil
}

// resolveBaseURL picks the endpoint and refuses one the platform will not send
// a credential to.
//
// Plain HTTP is accepted only on loopback, where a local development gateway
// lives. Anywhere else it would put the key on the wire in clear text, and a
// key is spendable - so the platform refuses rather than warns.
func resolveBaseURL(provider, raw string) (string, error) {
	value := strings.TrimSpace(raw)
	if value == "" {
		preset, known := providerBaseURLs[provider]
		if !known {
			return "", fmt.Errorf("llm: provider %q has no default endpoint; set AI_BASE_URL", provider)
		}
		value = preset
	}
	value = strings.TrimSuffix(value, "/")

	parsed, err := url.Parse(value)
	if err != nil {
		return "", fmt.Errorf("llm: invalid base URL: %w", err)
	}
	switch parsed.Scheme {
	case "https":
	case "http":
		host := strings.ToLower(parsed.Hostname())
		if host != "127.0.0.1" && host != "::1" && host != "localhost" {
			return "", errors.New("llm: a plain HTTP endpoint is only accepted on loopback: the key would travel in clear text")
		}
	default:
		return "", fmt.Errorf("llm: the endpoint must be http or https, got %q", parsed.Scheme)
	}
	if parsed.Host == "" {
		return "", errors.New("llm: the endpoint must include a host")
	}
	return value, nil
}

// Available reports whether a model is configured.
func (c *Client) Available() bool {
	return c != nil && c.apiKey != "" && c.model != "" && c.baseURL != ""
}

// Message is one chat turn on the wire.
type chatMessage struct {
	Role    string `json:"role"`
	Content string `json:"content"`
}

type toolFunction struct {
	Name        string          `json:"name"`
	Description string          `json:"description"`
	Parameters  json.RawMessage `json:"parameters"`
}

type wireTool struct {
	Type     string       `json:"type"`
	Function toolFunction `json:"function"`
}

type chatRequestBody struct {
	Model      string        `json:"model"`
	Messages   []chatMessage `json:"messages"`
	Tools      []wireTool    `json:"tools,omitempty"`
	ToolChoice string        `json:"tool_choice,omitempty"`
}

type chatResponseBody struct {
	Model   string `json:"model"`
	Choices []struct {
		FinishReason string `json:"finish_reason"`
		Message      struct {
			Content   string `json:"content"`
			ToolCalls []struct {
				ID       string `json:"id"`
				Function struct {
					Name      string `json:"name"`
					Arguments string `json:"arguments"`
				} `json:"function"`
			} `json:"tool_calls"`
		} `json:"message"`
	} `json:"choices"`
	Error *struct {
		Message string `json:"message"`
	} `json:"error"`
}

// Chat performs one completion.
func (c *Client) Chat(ctx context.Context, request agent.ChatRequest) (agent.ChatResponse, error) {
	if !c.Available() {
		return agent.ChatResponse{}, errors.New("llm: no model is configured")
	}

	body := chatRequestBody{Model: c.model, Messages: make([]chatMessage, 0, len(request.Messages))}
	for _, message := range request.Messages {
		body.Messages = append(body.Messages, chatMessage{
			Role:    string(message.Role),
			Content: message.Content,
		})
	}
	if len(request.Tools) > 0 {
		body.Tools = make([]wireTool, 0, len(request.Tools))
		for _, tool := range request.Tools {
			parameters := json.RawMessage(tool.ParametersJSON)
			// A schema that does not parse would make the provider reject the
			// whole request, so the tool is offered with an empty object schema
			// instead. The tool still validates its own arguments when it runs.
			if len(parameters) == 0 || !json.Valid(parameters) {
				parameters = json.RawMessage(`{"type":"object"}`)
			}
			body.Tools = append(body.Tools, wireTool{
				Type: "function",
				Function: toolFunction{
					Name:        tool.Name,
					Description: tool.Description,
					Parameters:  parameters,
				},
			})
		}
		body.ToolChoice = "auto"
	}

	payload, err := json.Marshal(body)
	if err != nil {
		return agent.ChatResponse{}, fmt.Errorf("llm: could not encode the request: %w", err)
	}

	ctx, cancel := context.WithTimeout(ctx, c.timeout)
	defer cancel()
	httpRequest, err := http.NewRequestWithContext(ctx, http.MethodPost, c.baseURL+completionsPath, bytes.NewReader(payload))
	if err != nil {
		return agent.ChatResponse{}, fmt.Errorf("llm: could not build the request: %w", err)
	}
	httpRequest.Header.Set("Content-Type", "application/json; charset=utf-8")
	httpRequest.Header.Set("Accept", "application/json")
	// The key travels in the header and nowhere else: not in the URL, not in the
	// body, and not in any log line this package writes.
	httpRequest.Header.Set("Authorization", "Bearer "+c.apiKey)

	response, err := c.http.Do(httpRequest)
	if err != nil {
		c.logger.Warn("llm: the model could not be reached", "error", err.Error())
		return agent.ChatResponse{}, fmt.Errorf("llm: %w", err)
	}
	defer func() { _ = response.Body.Close() }()

	raw, err := io.ReadAll(io.LimitReader(response.Body, maximumResponseBytes+1))
	if err != nil {
		return agent.ChatResponse{}, fmt.Errorf("llm: could not read the response: %w", err)
	}
	if len(raw) > maximumResponseBytes {
		return agent.ChatResponse{}, errors.New("llm: the response was too large")
	}
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		// The provider's body can quote the request, so only the status is kept
		// in the log and nothing of the body reaches the client.
		c.logger.Warn("llm: the model answered with a status", "status", response.StatusCode)
		return agent.ChatResponse{}, fmt.Errorf("llm: the provider answered %d", response.StatusCode)
	}
	return parseChatResponse(raw, c.logger)
}

// parseChatResponse reads one completion.
func parseChatResponse(raw []byte, logger *slog.Logger) (agent.ChatResponse, error) {
	var body chatResponseBody
	if err := json.Unmarshal(raw, &body); err != nil {
		return agent.ChatResponse{}, errors.New("llm: the response was not JSON")
	}
	if body.Error != nil {
		// The provider's message is kept for the log and never forwarded: it can
		// name the endpoint and quote the request.
		logger.Warn("llm: the provider reported an error", "message", body.Error.Message)
		return agent.ChatResponse{}, errors.New("llm: the provider reported an error")
	}
	if len(body.Choices) == 0 {
		return agent.ChatResponse{}, errors.New("llm: the response carried no choices")
	}

	choice := body.Choices[0]
	result := agent.ChatResponse{Content: strings.TrimSpace(choice.Message.Content)}
	for _, call := range choice.Message.ToolCalls {
		name := strings.TrimSpace(call.Function.Name)
		if name == "" {
			return agent.ChatResponse{}, errors.New("llm: a tool call named no function")
		}
		result.ToolCalls = append(result.ToolCalls, agent.ToolCall{
			Name:          name,
			ArgumentsJSON: sanitizeArguments(call.Function.Arguments),
		})
	}
	if result.Content == "" && len(result.ToolCalls) == 0 {
		return agent.ChatResponse{}, errors.New("llm: the response carried neither content nor tool calls")
	}
	return result, nil
}

// sanitizeArguments cleans up the argument string a model produced.
//
// Models occasionally wrap the object in a Markdown code fence. Removing the
// fence is a formatting fix; the result is still handed to a strict JSON
// decoder by the caller, and nothing is guessed - a model that produced a
// truncated object gets no arguments rather than invented ones.
func sanitizeArguments(raw string) string {
	text := strings.TrimSpace(raw)
	if !strings.HasPrefix(text, "```") {
		return text
	}
	if newline := strings.IndexByte(text, '\n'); newline >= 0 {
		text = text[newline+1:]
	}
	text = strings.TrimSpace(strings.TrimSuffix(strings.TrimSpace(text), "```"))
	return text
}
