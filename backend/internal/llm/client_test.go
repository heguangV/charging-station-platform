package llm

import (
	"context"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/heguangV/charging-station-platform/backend/internal/agent"
)

// The model adapter is optional, so its two most important behaviours are that
// an unconfigured deployment is a supported one and that a configured one sends
// exactly what an OpenAI-compatible endpoint expects.

type stubModel struct {
	status int
	body   string
	header http.Header
	bodyIn []byte
	path   string
	calls  int
}

func (s *stubModel) handler() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		s.calls++
		s.header = r.Header.Clone()
		s.path = r.URL.Path
		s.bodyIn, _ = io.ReadAll(r.Body)
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(s.status)
		_, _ = io.WriteString(w, s.body)
	}
}

func newStubClient(t *testing.T, stub *stubModel, cfg Config) *Client {
	t.Helper()
	server := httptest.NewServer(stub.handler())
	t.Cleanup(server.Close)
	cfg.BaseURL = server.URL
	cfg.HTTPClient = server.Client()
	cfg.Logger = slog.New(slog.NewTextHandler(io.Discard, nil))
	client, err := New(cfg)
	if err != nil {
		t.Fatalf("New() error = %v", err)
	}
	return client
}

// —— configuration ——

// A deployment with no model is supported: the assistant answers from the
// deterministic path, and no error is raised for a feature that is simply off.
func TestNewWithoutAModelIsASupportedDeployment(t *testing.T) {
	cases := map[string]Config{
		"no model": {APIKey: "key"},
		"no key":   {Model: "gpt-4o-mini"},
		"neither":  {},
		"blank":    {Model: "  ", APIKey: "  "},
	}
	for name, cfg := range cases {
		t.Run(name, func(t *testing.T) {
			client, err := New(cfg)
			if err != nil {
				t.Fatalf("New() error = %v, want an unconfigured client", err)
			}
			if client.Available() {
				t.Fatal("a client without a model and a key must report itself unavailable")
			}
			if _, err := client.Chat(context.Background(), agent.ChatRequest{}); err == nil {
				t.Fatal("an unavailable client must not be callable")
			}
		})
	}
}

// A provider with a preset resolves to that endpoint, so an operator only has
// to name the provider and the model.
func TestNewResolvesTheProviderPreset(t *testing.T) {
	cases := map[string]string{
		"openai":   "https://api.openai.com/v1",
		"deepseek": "https://api.deepseek.com/v1",
		"qwen":     "https://dashscope.aliyuncs.com/compatible-mode/v1",
		"claude":   "https://api.anthropic.com/v1",
		"":         "https://api.openai.com/v1",
	}
	for provider, want := range cases {
		t.Run(provider, func(t *testing.T) {
			client, err := New(Config{Provider: provider, Model: "m", APIKey: "k"})
			if err != nil {
				t.Fatalf("New() error = %v", err)
			}
			if client.baseURL != want {
				t.Fatalf("baseURL = %q, want %q", client.baseURL, want)
			}
		})
	}
}

// A provider the platform has no preset for must name its endpoint: guessing
// would send a credential to the wrong host.
func TestNewRequiresAnEndpointForACustomProvider(t *testing.T) {
	if _, err := New(Config{Provider: "custom", Model: "m", APIKey: "k"}); err == nil {
		t.Fatal("expected a custom provider without an endpoint to be refused")
	}
	client, err := New(Config{Provider: "custom", Model: "m", APIKey: "k", BaseURL: "https://gateway.internal/v1"})
	if err != nil {
		t.Fatalf("New() error = %v", err)
	}
	if !client.Available() {
		t.Fatal("a custom provider with an endpoint must be usable")
	}
}

// A credential must not travel in clear text anywhere but loopback, where a
// local development gateway lives.
func TestNewRefusesAnInsecureEndpointOffLoopback(t *testing.T) {
	allowed := []string{
		"https://gateway.example/v1",
		"http://127.0.0.1:11434/v1",
		"http://localhost:11434/v1",
		"http://[::1]:11434/v1",
	}
	for _, endpoint := range allowed {
		if _, err := New(Config{Model: "m", APIKey: "k", BaseURL: endpoint}); err != nil {
			t.Fatalf("New(%q) error = %v, want it accepted", endpoint, err)
		}
	}
	refused := []string{
		"http://gateway.example/v1",
		"http://10.0.0.5:8000/v1",
		"ftp://gateway.example/v1",
		"gateway.example/v1",
	}
	for _, endpoint := range refused {
		if _, err := New(Config{Model: "m", APIKey: "k", BaseURL: endpoint}); err == nil {
			t.Fatalf("New(%q) was accepted, want it refused", endpoint)
		}
	}
}

// The timeout is clamped, so a misconfigured value cannot hold a request open.
func TestNewClampsTheTimeout(t *testing.T) {
	cases := map[time.Duration]time.Duration{
		0:                defaultTimeout,
		time.Millisecond: minimumTimeout,
		time.Hour:        maximumTimeout,
		5 * time.Second:  5 * time.Second,
	}
	for input, want := range cases {
		client, err := New(Config{Model: "m", APIKey: "k", Timeout: input})
		if err != nil {
			t.Fatalf("New() error = %v", err)
		}
		if client.timeout != want {
			t.Fatalf("timeout for %s = %s, want %s", input, client.timeout, want)
		}
	}
}

// —— the request ——

// Everything an OpenAI-compatible endpoint needs, in the shape it expects.
func TestChatSendsAnOpenAiCompatibleRequest(t *testing.T) {
	stub := &stubModel{status: http.StatusOK, body: `{"choices":[{"message":{"content":"好的"}}]}`}
	client := newStubClient(t, stub, Config{Model: "gpt-4o-mini", APIKey: "secret-key"})

	_, err := client.Chat(context.Background(), agent.ChatRequest{
		Messages: []agent.Message{
			{Role: agent.RoleSystem, Content: "你是助手。"},
			{Role: agent.RoleUser, Content: "附近哪有充电站"},
		},
		Tools: []agent.ToolSpec{{
			Name:           "station_search",
			Description:    "检索附近充电站",
			ParametersJSON: `{"type":"object","properties":{"limit":{"type":"integer"}}}`,
		}},
	})
	if err != nil {
		t.Fatalf("Chat() error = %v", err)
	}
	if stub.path != completionsPath {
		t.Fatalf("path = %q, want %q", stub.path, completionsPath)
	}
	// The credential travels in the header, never in the URL or the body.
	if got := stub.header.Get("Authorization"); got != "Bearer secret-key" {
		t.Fatalf("Authorization = %q", got)
	}
	if got := stub.header.Get("Content-Type"); !strings.HasPrefix(got, "application/json") {
		t.Fatalf("Content-Type = %q", got)
	}

	var body struct {
		Model      string `json:"model"`
		ToolChoice string `json:"tool_choice"`
		Messages   []struct {
			Role    string `json:"role"`
			Content string `json:"content"`
		} `json:"messages"`
		Tools []struct {
			Type     string `json:"type"`
			Function struct {
				Name       string          `json:"name"`
				Parameters json.RawMessage `json:"parameters"`
			} `json:"function"`
		} `json:"tools"`
	}
	if err := json.Unmarshal(stub.bodyIn, &body); err != nil {
		t.Fatalf("the request body is not JSON: %s", stub.bodyIn)
	}
	if body.Model != "gpt-4o-mini" {
		t.Fatalf("model = %q", body.Model)
	}
	if len(body.Messages) != 2 || body.Messages[0].Role != "system" || body.Messages[1].Role != "user" {
		t.Fatalf("messages = %+v, want the system turn first", body.Messages)
	}
	if len(body.Tools) != 1 || body.Tools[0].Type != "function" || body.Tools[0].Function.Name != "station_search" {
		t.Fatalf("tools = %+v", body.Tools)
	}
	if !json.Valid(body.Tools[0].Function.Parameters) {
		t.Fatalf("parameters must be a JSON object, got %s", body.Tools[0].Function.Parameters)
	}
	if body.ToolChoice != "auto" {
		t.Fatalf("tool_choice = %q, want auto", body.ToolChoice)
	}
	if strings.Contains(string(stub.bodyIn), "secret-key") {
		t.Fatal("the key must never appear in the request body")
	}
}

// A schema that does not parse would make the provider reject the entire
// request, so the tool is offered with an empty schema instead. The tool still
// validates its own arguments when it runs.
func TestChatOffersAnUnparsableSchemaAsAnEmptyObject(t *testing.T) {
	stub := &stubModel{status: http.StatusOK, body: `{"choices":[{"message":{"content":"好的"}}]}`}
	client := newStubClient(t, stub, Config{Model: "m", APIKey: "k"})

	_, err := client.Chat(context.Background(), agent.ChatRequest{
		Messages: []agent.Message{{Role: agent.RoleUser, Content: "hi"}},
		Tools:    []agent.ToolSpec{{Name: "broken", Description: "d", ParametersJSON: "{not json"}},
	})
	if err != nil {
		t.Fatalf("Chat() error = %v", err)
	}
	if !strings.Contains(string(stub.bodyIn), `"parameters":{"type":"object"}`) {
		t.Fatalf("body = %s", stub.bodyIn)
	}
}

// A model call with no tools is the second call, which asks for wording rather
// than for more work - and must not offer tools again.
func TestChatOmitsTheToolListWhenThereAreNoTools(t *testing.T) {
	stub := &stubModel{status: http.StatusOK, body: `{"choices":[{"message":{"content":"好的"}}]}`}
	client := newStubClient(t, stub, Config{Model: "m", APIKey: "k"})

	if _, err := client.Chat(context.Background(), agent.ChatRequest{
		Messages: []agent.Message{{Role: agent.RoleUser, Content: "hi"}},
	}); err != nil {
		t.Fatalf("Chat() error = %v", err)
	}
	if strings.Contains(string(stub.bodyIn), "tools") || strings.Contains(string(stub.bodyIn), "tool_choice") {
		t.Fatalf("body = %s, want no tool members", stub.bodyIn)
	}
}

// —— the response ——

func TestChatReadsTheReply(t *testing.T) {
	stub := &stubModel{status: http.StatusOK, body: `{"model":"gpt-4o-mini","choices":[{"finish_reason":"stop","message":{"content":"  为你找到 3 个充电站。  "}}]}`}
	client := newStubClient(t, stub, Config{Model: "m", APIKey: "k"})

	response, err := client.Chat(context.Background(), agent.ChatRequest{
		Messages: []agent.Message{{Role: agent.RoleUser, Content: "hi"}},
	})
	if err != nil {
		t.Fatalf("Chat() error = %v", err)
	}
	if response.Content != "为你找到 3 个充电站。" {
		t.Fatalf("content = %q, want it trimmed", response.Content)
	}
}

func TestChatReadsToolCalls(t *testing.T) {
	stub := &stubModel{status: http.StatusOK, body: `{"choices":[{"message":{"content":null,"tool_calls":[
        {"id":"call_1","function":{"name":"station_search","arguments":"{\"limit\":3}"}}
    ]}}]}`}
	client := newStubClient(t, stub, Config{Model: "m", APIKey: "k"})

	response, err := client.Chat(context.Background(), agent.ChatRequest{
		Messages: []agent.Message{{Role: agent.RoleUser, Content: "hi"}},
	})
	if err != nil {
		t.Fatalf("Chat() error = %v", err)
	}
	if len(response.ToolCalls) != 1 {
		t.Fatalf("tool calls = %+v", response.ToolCalls)
	}
	if response.ToolCalls[0].Name != "station_search" || response.ToolCalls[0].ArgumentsJSON != `{"limit":3}` {
		t.Fatalf("tool call = %+v", response.ToolCalls[0])
	}
}

// Models occasionally wrap an argument object in a Markdown fence. Removing the
// fence is a formatting fix; nothing is guessed, and a truncated object is left
// for the caller's strict decoder to reject.
func TestChatStripsACodeFenceFromArguments(t *testing.T) {
	stub := &stubModel{status: http.StatusOK, body: `{"choices":[{"message":{"tool_calls":[
        {"function":{"name":"station_search","arguments":"\u0060\u0060\u0060json\n{\"limit\":3}\n\u0060\u0060\u0060"}}
    ]}}]}`}
	client := newStubClient(t, stub, Config{Model: "m", APIKey: "k"})

	response, err := client.Chat(context.Background(), agent.ChatRequest{
		Messages: []agent.Message{{Role: agent.RoleUser, Content: "hi"}},
	})
	if err != nil {
		t.Fatalf("Chat() error = %v", err)
	}
	if response.ToolCalls[0].ArgumentsJSON != `{"limit":3}` {
		t.Fatalf("arguments = %q, want the fence removed", response.ToolCalls[0].ArgumentsJSON)
	}
}

// Every way a provider can fail to answer has to become an error the assistant
// can degrade on, rather than a panic or a silent empty reply.
func TestChatReportsEveryUnusableResponse(t *testing.T) {
	cases := map[string]struct {
		status int
		body   string
	}{
		"http error":            {status: http.StatusUnauthorized, body: `{"error":{"message":"bad key"}}`},
		"provider error object": {status: http.StatusOK, body: `{"error":{"message":"quota exceeded"}}`},
		"no choices":            {status: http.StatusOK, body: `{"choices":[]}`},
		"empty message":         {status: http.StatusOK, body: `{"choices":[{"message":{"content":"   "}}]}`},
		"not json":              {status: http.StatusOK, body: `not json at all`},
		"nameless tool call":    {status: http.StatusOK, body: `{"choices":[{"message":{"tool_calls":[{"function":{"arguments":"{}"}}]}}]}`},
	}
	for name, tc := range cases {
		t.Run(name, func(t *testing.T) {
			client := newStubClient(t, &stubModel{status: tc.status, body: tc.body}, Config{Model: "m", APIKey: "k"})
			if _, err := client.Chat(context.Background(), agent.ChatRequest{
				Messages: []agent.Message{{Role: agent.RoleUser, Content: "hi"}},
			}); err == nil {
				t.Fatal("expected the response to be refused")
			}
		})
	}
}

// The provider's error text can quote the request or name the endpoint, so it is
// kept for the log and never returned to the caller.
func TestChatDoesNotLeakTheProviderErrorText(t *testing.T) {
	client := newStubClient(t, &stubModel{
		status: http.StatusBadRequest,
		body:   `{"error":{"message":"invalid key sk-live-abcdef for https://api.example/v1"}}`,
	}, Config{Model: "m", APIKey: "k"})

	_, err := client.Chat(context.Background(), agent.ChatRequest{
		Messages: []agent.Message{{Role: agent.RoleUser, Content: "hi"}},
	})
	if err == nil {
		t.Fatal("expected an error")
	}
	if strings.Contains(err.Error(), "sk-live-abcdef") || strings.Contains(err.Error(), "api.example") {
		t.Fatalf("the error must not quote the provider's text: %v", err)
	}
}
