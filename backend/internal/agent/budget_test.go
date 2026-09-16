package agent

import (
	"context"
	"encoding/json"
	"io"
	"log/slog"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/heguangV/charging-station-platform/backend/internal/auth"
	"github.com/heguangV/charging-station-platform/backend/internal/config"
	"github.com/heguangV/charging-station-platform/backend/internal/httpapi"
	"github.com/heguangV/charging-station-platform/backend/internal/station"
)

type blockingModel struct {
	calls     int
	blockPlan bool
}

func (*blockingModel) Available() bool { return true }
func (m *blockingModel) Chat(ctx context.Context, _ ChatRequest) (ChatResponse, error) {
	m.calls++
	if m.blockPlan || m.calls > 1 {
		<-ctx.Done()
		return ChatResponse{}, ctx.Err()
	}
	return ChatResponse{}, nil
}

type blockingDirectory struct{ fakeStations }

func (d *blockingDirectory) SearchSummaries(ctx context.Context, _ station.SummaryFilter) ([]station.Summary, error) {
	<-ctx.Done()
	return nil, ctx.Err()
}

func TestChatBudget(t *testing.T) {
	for _, planning := range []bool{true, false} {
		name := "composition"
		if planning {
			name = "planning"
		}
		t.Run(name, func(t *testing.T) {
			directory := &fakeStations{summaries: []station.Summary{sampleStation(1, "真实站点")}}
			model := &blockingModel{blockPlan: planning}
			service := newTestService(t, Config{Stations: directory, LLM: model})
			service.budget = 200 * time.Millisecond
			service.planningBudget = 30 * time.Millisecond
			start := time.Now()
			result := service.Chat(context.Background(), "附近充电站", Context{})
			if time.Since(start) > time.Second {
				t.Fatal("budget was not enforced")
			}
			if !result.Degraded || result.LLMUsed || len(result.Stations) != 1 || !strings.Contains(result.Reply, timeoutNotice) {
				t.Fatalf("lost degraded station result: %+v", result)
			}
			wantCalls := 2
			if planning {
				wantCalls = 1
			}
			if model.calls != wantCalls {
				t.Fatalf("model calls=%d", model.calls)
			}
		})
	}
	t.Run("cancelled caller starts no work", func(t *testing.T) {
		directory := &fakeStations{}
		model := &blockingModel{}
		service := newTestService(t, Config{Stations: directory, LLM: model})
		ctx, cancel := context.WithCancel(context.Background())
		cancel()
		result := service.Chat(ctx, "充电桩详情", Context{})
		if model.calls != 0 || len(directory.searches) != 0 || len(directory.detailIDs) != 0 || !result.Degraded {
			t.Fatalf("work after cancellation: %+v", result)
		}
	})
	t.Run("caller cancellation stops an active model", func(t *testing.T) {
		directory := &fakeStations{}
		model := &notifyingModel{started: make(chan struct{})}
		service := newTestService(t, Config{Stations: directory, LLM: model})
		ctx, cancel := context.WithCancel(context.Background())
		defer cancel()
		done := make(chan Result, 1)
		go func() { done <- service.Chat(ctx, "附近充电站", Context{}) }()
		select {
		case <-model.started:
		case <-time.After(time.Second):
			t.Fatal("model did not start")
		}
		cancel()
		select {
		case result := <-done:
			if !result.Degraded || len(directory.searches) != 0 || !strings.Contains(result.Reply, timeoutNotice) {
				t.Fatalf("continued work after cancellation: %+v", result)
			}
		case <-time.After(time.Second):
			t.Fatal("caller cancellation was ignored")
		}
	})
	t.Run("slow tool skips following tools", func(t *testing.T) {
		directory := &blockingDirectory{}
		service := newTestService(t, Config{Stations: directory})
		service.budget = 30 * time.Millisecond
		result := service.Chat(context.Background(), "充电桩详情", Context{})
		if !result.Degraded || len(directory.detailIDs) != 0 || !strings.Contains(result.Reply, timeoutNotice) {
			t.Fatalf("bad tool timeout: %+v", result)
		}
	})
}

func TestChatBudgetHTTP(t *testing.T) {
	service := newTestService(t, Config{Stations: &fakeStations{summaries: []station.Summary{sampleStation(1, "真实站点")}}, LLM: &blockingModel{}})
	service.budget = 100 * time.Millisecond
	handlers, err := NewHandlers(service, &testAuth{identity: auth.Identity{ID: 42, Role: auth.RoleUser}})
	if err != nil {
		t.Fatal(err)
	}
	api := httpapi.NewServer(config.Config{RequestIDHeader: "X-Request-ID"}, slog.New(slog.NewTextHandler(io.Discard, nil)))
	handlers.Register(api)
	server := httptest.NewUnstartedServer(api.Handler())
	// Exercise ResponseController through the production response writer wrapper.
	server.Config.WriteTimeout = 20 * time.Millisecond
	server.Start()
	defer server.Close()
	client := server.Client()
	client.Timeout = 2 * time.Second
	response, err := client.Post(server.URL+ChatPath, "application/json", strings.NewReader(`{"message":"附近充电站"}`))
	if err != nil {
		t.Fatal(err)
	}
	defer response.Body.Close()
	var envelope struct {
		Success bool `json:"success"`
		Data    struct {
			Degraded bool              `json:"degraded"`
			Stations []json.RawMessage `json:"stations"`
		} `json:"data"`
	}
	if err := json.NewDecoder(response.Body).Decode(&envelope); err != nil {
		t.Fatal(err)
	}
	if response.StatusCode != 200 || !envelope.Success || !envelope.Data.Degraded || len(envelope.Data.Stations) != 1 {
		t.Fatalf("unexpected response: %+v", envelope)
	}
}

// Signals the test only after work starts; cancellation must propagate to it.
type notifyingModel struct{ started chan struct{} }

func (*notifyingModel) Available() bool { return true }
func (m *notifyingModel) Chat(ctx context.Context, _ ChatRequest) (ChatResponse, error) {
	close(m.started)
	<-ctx.Done()
	return ChatResponse{}, ctx.Err()
}
