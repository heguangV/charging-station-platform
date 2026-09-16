package order

import (
	"crypto/sha256"
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/heguangV/charging-station-platform/backend/internal/httpapi"
)

// The internal charger event receipt endpoint (BE-I-02).
//
// This is where the physical facts of a charge enter the platform. The order service was always
// able to apply them - ConfirmStart and ConfirmStop write the state, the bill and the outbox event
// in one transaction - but nothing called it, so the P0 flow could not get past STARTING. The
// gateway calls this endpoint with a service token, and what it reports is treated as the truth
// about the device: a start receipt is what moves an order to CHARGING, and a stop receipt is what
// completes it and releases the charger.
//
// Two properties matter more than anything else here. The endpoint is authenticated, because a
// forged receipt advances an order and starts a bill. And it is idempotent on the receipt id,
// because a gateway that does not get an answer will send the same fact again, and applying a
// charge twice would bill it twice.

const (
	// ChargerEventPath is the internal route the charger gateway posts to.
	ChargerEventPath = "/api/v1/internal/charger-events"

	// chargerEventStarted and chargerEventStopped are the receipt types of the
	// frozen contract. Nothing else may reach the order service.
	chargerEventStarted = "CHARGE_STARTED"
	chargerEventStopped = "CHARGE_STOPPED"
	// ChargerEventProgress is the running-meter receipt a charger posts while it is charging.
	// Unlike the start and stop facts it publishes nothing: it updates the order's metered
	// reading, and the settled bill still comes from the stop receipt.
	ChargerEventProgress = "CHARGE_PROGRESS"
)

// ChargerEventConfig configures the receipt endpoint.
type ChargerEventConfig struct {
	// Token is the service credential the gateway presents (NCS_CHARGER_GATEWAY_TOKEN).
	// It has no default: a missing token would leave a public endpoint that can
	// advance any order.
	Token string
	// MaxFutureSkew bounds how far a device fact time may run ahead of the server
	// clock. Zero keeps the service default.
	MaxFutureSkew time.Duration
	// Logger receives the audit line for every accepted and every refused receipt.
	Logger *slog.Logger
}

// ChargerEventHandlers serves the gateway's receipts.
type ChargerEventHandlers struct {
	service *Service
	token   string
	logger  *slog.Logger
}

// NewChargerEventHandlers binds the endpoint to the order service and the service token.
//
// The fact-time skew is applied to the service here, so the receipt policy lives in one place
// instead of being spread over the process wiring.
func NewChargerEventHandlers(service *Service, cfg ChargerEventConfig) (*ChargerEventHandlers, error) {
	if service == nil {
		return nil, errors.New("order: charger event handlers require a service")
	}
	token := strings.TrimSpace(cfg.Token)
	if token == "" {
		return nil, errors.New("order: NCS_CHARGER_GATEWAY_TOKEN is required: the receipt endpoint advances orders and billing")
	}
	if cfg.MaxFutureSkew > 0 {
		if err := service.SetFactTimeSkew(cfg.MaxFutureSkew); err != nil {
			return nil, err
		}
	}
	logger := cfg.Logger
	if logger == nil {
		logger = slog.New(slog.NewTextHandler(io.Discard, nil))
	}
	return &ChargerEventHandlers{service: service, token: token, logger: logger}, nil
}

// Register attaches the internal receipt route.
func (h *ChargerEventHandlers) Register(server interface {
	Register(pattern string, handler http.HandlerFunc)
}) {
	server.Register(ChargerEventPath, h.chargerEvents)
}

// chargerEventRequest is the receipt as the gateway sends it. The payload is
// validated field by field rather than trusted: every field here decides an
// order's state or its bill.
type chargerEventRequest struct {
	EventID    string         `json:"eventId"`
	EventType  string         `json:"eventType"`
	OrderNo    string         `json:"orderNo"`
	ChargerID  chargerIDField `json:"chargerId"`
	OccurredAt time.Time      `json:"occurredAt"`
	// EnergyWh is required for a stop: the metered energy is what the bill is
	// computed from, and a stop without it cannot complete an order.
	EnergyWh     *int64 `json:"energyWh"`
	MeterStartWh *int64 `json:"meterStartWh"`
	MeterEndWh   *int64 `json:"meterEndWh"`
	TraceID      string `json:"traceId"`
}

// chargerIDField accepts the charger identifier either as a JSON number or as
// the string form the device command carries, because the gateway echoes back
// the identifier the platform sent it.
type chargerIDField struct {
	value int64
}

func (f *chargerIDField) UnmarshalJSON(raw []byte) error {
	text := strings.Trim(strings.TrimSpace(string(raw)), `"`)
	if text == "" || text == "null" {
		return nil
	}
	value, err := strconv.ParseInt(text, 10, 64)
	if err != nil {
		return fmt.Errorf("chargerId must be the numeric charger id: %w", err)
	}
	f.value = value
	return nil
}

// chargerEvents handles POST /api/v1/internal/charger-events.
func (h *ChargerEventHandlers) chargerEvents(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodPost) {
		return
	}
	if !h.authorized(r) {
		// A refused receipt is worth a log line: it is either a misconfigured
		// gateway or somebody trying to advance orders without a device.
		h.logger.Warn("charger event refused",
			"reason", "service token is missing or invalid",
			"request_id", httpapi.RequestID(r.Context()),
			"remote_addr", r.RemoteAddr)
		httpapi.WriteError(w, r, http.StatusUnauthorized, httpapi.CodeUnauthorized, "service token is missing or invalid", nil)
		return
	}

	body, ok := readBody(w, r)
	if !ok {
		return
	}
	var request chargerEventRequest
	if err := json.Unmarshal(body, &request); err != nil {
		h.refuse(w, r, "invalid request body", nil)
		return
	}

	eventType := strings.ToUpper(strings.TrimSpace(request.EventType))
	occurredAt := request.OccurredAt.UTC().Format(time.RFC3339Nano)
	traceID := strings.TrimSpace(request.TraceID)
	if traceID == "" {
		traceID = httpapi.RequestID(r.Context())
	}

	switch eventType {
	case chargerEventStarted:
		result, err := h.service.ConfirmStart(r.Context(), ConfirmStartCommand{
			OrderNo:     strings.TrimSpace(request.OrderNo),
			ChargerID:   request.ChargerID.value,
			OccurredAt:  request.OccurredAt,
			EventID:     strings.TrimSpace(request.EventID),
			RequestHash: chargerEventDigest(eventType, request),
			TraceID:     traceID,
		})
		h.respond(w, r, eventType, request, occurredAt, result, err)
	case ChargerEventProgress:
		// The running meter is the whole point of the receipt; without it there is nothing to apply.
		if request.EnergyWh == nil {
			h.refuse(w, r, "energyWh is required for CHARGE_PROGRESS", nil)
			return
		}
		if *request.EnergyWh < 0 {
			h.refuse(w, r, "energyWh must not be negative", nil)
			return
		}
		result, err := h.service.ConfirmProgress(r.Context(), ConfirmProgressCommand{
			OrderNo:     strings.TrimSpace(request.OrderNo),
			ChargerID:   request.ChargerID.value,
			EnergyWh:    *request.EnergyWh,
			OccurredAt:  request.OccurredAt,
			EventID:     strings.TrimSpace(request.EventID),
			RequestHash: chargerEventDigest(eventType, request),
			TraceID:     traceID,
		})
		h.respond(w, r, eventType, request, occurredAt, result, err)
	case chargerEventStopped:
		if request.EnergyWh == nil {
			h.refuse(w, r, "energyWh is required for CHARGE_STOPPED", nil)
			return
		}
		if *request.EnergyWh < 0 {
			h.refuse(w, r, "energyWh must not be negative", nil)
			return
		}
		result, err := h.service.ConfirmStop(r.Context(), ConfirmStopCommand{
			OrderNo:      strings.TrimSpace(request.OrderNo),
			ChargerID:    request.ChargerID.value,
			EnergyWh:     *request.EnergyWh,
			MeterStartWh: request.MeterStartWh,
			MeterEndWh:   request.MeterEndWh,
			OccurredAt:   request.OccurredAt,
			EventID:      strings.TrimSpace(request.EventID),
			RequestHash:  chargerEventDigest(eventType, request),
			TraceID:      traceID,
		})
		h.respond(w, r, eventType, request, occurredAt, result, err)
	default:
		h.refuse(w, r, "eventType must be "+chargerEventStarted+", "+chargerEventStopped+" or "+ChargerEventProgress, nil)
	}
}

// respond writes the result of an applied receipt.
//
// A duplicate delivery is answered with the first result rather than a
// different one, which is what lets the gateway retry until it is told the fact
// was recorded.
func (h *ChargerEventHandlers) respond(w http.ResponseWriter, r *http.Request, eventType string, request chargerEventRequest, occurredAt string, result Order, err error) {
	if err != nil {
		h.refuse(w, r, err.Error(), err)
		return
	}
	h.logger.Info("charger event applied",
		"event_id", strings.TrimSpace(request.EventID),
		"event_type", eventType,
		"order_no", result.OrderNo,
		"order_status", result.Status,
		"charger_id", request.ChargerID.value,
		"occurred_at", occurredAt,
		"trace_id", httpapi.RequestID(r.Context()))
	httpapi.WriteJSON(w, http.StatusOK, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data: map[string]any{
			"eventId":    strings.TrimSpace(request.EventID),
			"eventType":  eventType,
			"orderNo":    result.OrderNo,
			"status":     result.Status,
			"occurredAt": occurredAt,
			"order":      result,
		},
	})
}

// refuse maps a domain error to the shared envelope and records the audit line.
//
// The status codes are the contract: 400 for a payload the platform cannot
// accept at all, 404 for an order that does not exist, 409 for a fact that
// contradicts the order - an out-of-order receipt, a receipt for another
// charger, or one whose fact time is earlier than the recorded start. None of
// them changes an order, and the store writes its own audit row for the 409s.
func (h *ChargerEventHandlers) refuse(w http.ResponseWriter, r *http.Request, reason string, err error) {
	h.logger.Warn("charger event refused",
		"reason", reason,
		"request_id", httpapi.RequestID(r.Context()))
	if err != nil {
		writeOrderError(w, r, err)
		return
	}
	httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, reason, nil)
}

// authorized checks the service token in constant time. The comparison is
// constant time so a wrong token cannot be recovered byte by byte from the
// response timing; the length is not hidden, which is not a secret worth hiding.
func (h *ChargerEventHandlers) authorized(r *http.Request) bool {
	header := strings.TrimSpace(r.Header.Get("Authorization"))
	parts := strings.SplitN(header, " ", 2)
	if len(parts) != 2 || !strings.EqualFold(parts[0], "Bearer") {
		return false
	}
	presented := strings.TrimSpace(parts[1])
	if presented == "" {
		return false
	}
	return subtle.ConstantTimeCompare([]byte(presented), []byte(h.token)) == 1
}

// chargerEventDigest fingerprints the receipt's meaning.
//
// It is deliberately not a hash of the raw bytes: a gateway retry may reorder
// fields or reformat the timestamp, and that is still the same fact, so it has
// to replay. A different fact under the same receipt id is a conflict, which the
// idempotency layer reports as 409.
func chargerEventDigest(eventType string, request chargerEventRequest) string {
	hasher := sha256.New()
	write := func(parts ...string) {
		for _, part := range parts {
			hasher.Write([]byte(part))
			hasher.Write([]byte{0})
		}
	}
	energy, meterStart, meterEnd := "-", "-", "-"
	if request.EnergyWh != nil {
		energy = strconv.FormatInt(*request.EnergyWh, 10)
	}
	if request.MeterStartWh != nil {
		meterStart = strconv.FormatInt(*request.MeterStartWh, 10)
	}
	if request.MeterEndWh != nil {
		meterEnd = strconv.FormatInt(*request.MeterEndWh, 10)
	}
	write(eventType,
		strings.TrimSpace(request.EventID),
		strings.TrimSpace(request.OrderNo),
		strconv.FormatInt(request.ChargerID.value, 10),
		request.OccurredAt.UTC().Format(time.RFC3339Nano),
		energy, meterStart, meterEnd)
	return hex.EncodeToString(hasher.Sum(nil))
}
