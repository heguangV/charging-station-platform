package httpapi

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/http"
	"strconv"
	"strings"
	"sync/atomic"
	"time"

	"github.com/heguangV/charging-station-platform/backend/internal/config"
)

const (
	CodeOK               = 0
	CodeNotReady         = 1001
	CodeMethodNotAllowed = 1002
	CodeNotFound         = 1003
	CodeInternalError    = 1500
)

// Business codes from the shared error-code registry (docs/database-api.md
// §1.10). The registry is append-only: these values already exist there, so
// business modules reuse them instead of minting new numbers.
const (
	CodeInvalidArgument  = 1   // INVALID_ARGUMENT, 400
	CodeDatabaseError    = 3   // DATABASE_ERROR, 503
	CodeResourceNotFound = 4   // NOT_FOUND, 404
	CodeUserFrozen       = 6   // USER_FROZEN, 403
	CodeRateLimited      = 19  // RATE_LIMITED, 429
	CodeUnauthorized     = 401 // UNAUTHORIZED, 401
	CodeForbidden        = 403 // FORBIDDEN, 403
	maxRequestIDLength   = 128
	requestIDByteLength  = 16
	maxPageSize          = 100
	defaultPageSize      = 20
	minPage              = 1
)

// ErrInvalidQueryParameter reports a query parameter outside its contract
// bounds; handlers map it to 400 INVALID_ARGUMENT.
var ErrInvalidQueryParameter = errors.New("httpapi: invalid query parameter")

type contextKey string

const requestIDContextKey contextKey = "request_id"

// Response is the common JSON envelope returned by the API. TraceID carries
// the request ID on error envelopes, matching the contract ErrorEnvelope.
type Response struct {
	Success bool   `json:"success"`
	Code    int    `json:"code"`
	Message string `json:"message"`
	Data    any    `json:"data,omitempty"`
	TraceID string `json:"traceId,omitempty"`
}

// Server owns the HTTP transport and the small set of cross-cutting concerns
// needed before business modules are attached.
type Server struct {
	config     config.Config
	logger     *slog.Logger
	ready      atomic.Bool
	mux        *http.ServeMux
	httpServer *http.Server
}

// NewServer creates an API server with only standard-library dependencies.
func NewServer(cfg config.Config, logger *slog.Logger) *Server {
	if logger == nil {
		logger = slog.New(slog.NewTextHandler(io.Discard, nil))
	}

	mux := http.NewServeMux()
	server := &Server{config: cfg, logger: logger, mux: mux}
	mux.HandleFunc("/healthz", server.healthz)
	mux.HandleFunc("/readyz", server.readyz)
	mux.HandleFunc("/", server.notFound)

	server.httpServer = &http.Server{
		Handler:           server.withMiddleware(mux),
		ReadHeaderTimeout: 5 * time.Second,
		IdleTimeout:       60 * time.Second,
		WriteTimeout:      15 * time.Second,
	}
	return server
}

// Handler returns the fully-wrapped handler for in-process and test servers.
func (s *Server) Handler() http.Handler {
	return s.httpServer.Handler
}

// Register attaches a business route onto the API mux. Patterns follow the
// Go 1.22 ServeMux syntax (path parameters supported). It must be called
// before Serve. Method enforcement and JSON error envelopes stay with the
// handlers so every response keeps the unified shape.
func (s *Server) Register(pattern string, handler http.HandlerFunc) {
	s.mux.Handle(pattern, handler)
}

// Serve runs the API on an already-bound listener. Binding before marking the
// service ready avoids advertising readiness when the port cannot be opened.
func (s *Server) Serve(listener net.Listener) error {
	return s.httpServer.Serve(listener)
}

// Shutdown stops accepting requests and waits for active handlers to finish.
func (s *Server) Shutdown(ctx context.Context) error {
	s.ready.Store(false)
	return s.httpServer.Shutdown(ctx)
}

// SetReady controls the process-level readiness state. Dependency checks are
// intentionally reserved for the database/Redis owning modules.
func (s *Server) SetReady(ready bool) {
	s.ready.Store(ready)
}

// RequestID returns the request ID stored by the middleware, if any.
func RequestID(ctx context.Context) string {
	requestID, _ := ctx.Value(requestIDContextKey).(string)
	return requestID
}

func (s *Server) withMiddleware(next http.Handler) http.Handler {
	return s.requestID(s.accessLog(next))
}

func (s *Server) requestID(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		requestID := strings.TrimSpace(r.Header.Get(s.config.RequestIDHeader))
		if !validRequestID(requestID) {
			requestID = newRequestID()
		}

		w.Header().Set(s.config.RequestIDHeader, requestID)
		ctx := context.WithValue(r.Context(), requestIDContextKey, requestID)
		next.ServeHTTP(w, r.WithContext(ctx))
	})
}

func (s *Server) accessLog(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		recorder := &statusRecorder{ResponseWriter: w}
		started := time.Now()
		next.ServeHTTP(recorder, r)
		s.logger.Info("http request",
			"request_id", RequestID(r.Context()),
			"method", r.Method,
			"path", r.URL.Path,
			"status", recorder.status,
			"duration_ms", time.Since(started).Milliseconds(),
		)
	})
}

func (s *Server) healthz(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		w.Header().Set("Allow", http.MethodGet)
		WriteError(w, r, http.StatusMethodNotAllowed, CodeMethodNotAllowed, "method not allowed", nil)
		return
	}

	WriteJSON(w, http.StatusOK, Response{
		Success: true,
		Code:    CodeOK,
		Message: "ok",
		Data: map[string]any{
			"service":   "api",
			"status":    "ok",
			"timestamp": time.Now().UTC().Format(time.RFC3339),
		},
	})
}

func (s *Server) readyz(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		w.Header().Set("Allow", http.MethodGet)
		WriteError(w, r, http.StatusMethodNotAllowed, CodeMethodNotAllowed, "method not allowed", nil)
		return
	}

	if !s.ready.Load() {
		WriteError(w, r, http.StatusServiceUnavailable, CodeNotReady, "service is not ready", map[string]any{"ready": false})
		return
	}

	WriteJSON(w, http.StatusOK, Response{
		Success: true,
		Code:    CodeOK,
		Message: "ok",
		Data:    map[string]any{"ready": true},
	})
}

func (s *Server) notFound(w http.ResponseWriter, r *http.Request) {
	WriteError(w, r, http.StatusNotFound, CodeNotFound, "resource not found", nil)
}

// WriteError emits a failed envelope. The trace ID carries the request ID so
// clients can quote it in support requests and log searches line up.
func WriteError(w http.ResponseWriter, r *http.Request, status, code int, message string, data any) {
	WriteJSON(w, status, Response{
		Success: false,
		Code:    code,
		Message: message,
		Data:    data,
		TraceID: RequestID(r.Context()),
	})
}

// WriteJSON emits a success envelope.
func WriteJSON(w http.ResponseWriter, status int, response Response) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	if err := json.NewEncoder(w).Encode(response); err != nil {
		return
	}
}

// ParsePagination reads the shared page/pageSize query parameters with the
// contract defaults (page 1, pageSize 20, maximum 100).
func ParsePagination(r *http.Request) (page, pageSize int64, err error) {
	page, err = parsePositiveInt(r.URL.Query().Get("page"), minPage)
	if err != nil {
		return 0, 0, err
	}
	pageSize, err = parsePositiveInt(r.URL.Query().Get("pageSize"), defaultPageSize)
	if err != nil {
		return 0, 0, err
	}
	if pageSize > maxPageSize {
		return 0, 0, ErrInvalidQueryParameter
	}
	return page, pageSize, nil
}

func parsePositiveInt(raw string, fallback int64) (int64, error) {
	raw = strings.TrimSpace(raw)
	if raw == "" {
		return fallback, nil
	}
	value, err := strconv.ParseInt(raw, 10, 64)
	if err != nil || value < minPage {
		return 0, ErrInvalidQueryParameter
	}
	return value, nil
}

func validRequestID(value string) bool {
	if value == "" || len(value) > maxRequestIDLength {
		return false
	}
	for _, char := range value {
		if (char >= 'a' && char <= 'z') ||
			(char >= 'A' && char <= 'Z') ||
			(char >= '0' && char <= '9') ||
			strings.ContainsRune("-_.:", char) {
			continue
		}
		return false
	}
	return true
}

func newRequestID() string {
	buffer := make([]byte, requestIDByteLength)
	if _, err := rand.Read(buffer); err == nil {
		return hex.EncodeToString(buffer)
	}
	return fmt.Sprintf("fallback-%d", time.Now().UnixNano())
}

type statusRecorder struct {
	http.ResponseWriter
	status int
}

// Unwrap lets ResponseController apply route-specific connection deadlines.
func (r *statusRecorder) Unwrap() http.ResponseWriter { return r.ResponseWriter }

func (r *statusRecorder) WriteHeader(status int) {
	if r.status != 0 {
		return
	}
	r.status = status
	r.ResponseWriter.WriteHeader(status)
}

func (r *statusRecorder) Write(body []byte) (int, error) {
	if r.status == 0 {
		r.WriteHeader(http.StatusOK)
	}
	return r.ResponseWriter.Write(body)
}
