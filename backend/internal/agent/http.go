package agent

import (
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"strings"

	"github.com/heguangV/charging-station-platform/backend/internal/auth"
	"github.com/heguangV/charging-station-platform/backend/internal/httpapi"
)

// The browser entry point to the assistant.
//
// It is one read-only POST and it is behind the user session: the assistant
// reads a user's account context, so an anonymous caller must not reach it, and
// an administrator's token must not either - the questions it answers are the
// C-end user's.
//
// Everything the browser must not hold stays behind this boundary: the map
// Server Key, the model credential, the tool orchestration and the degradation
// policy. The client sends a question, its own position and an optional
// preference, and nothing else.

// ChatPath is the route the browser posts to.
const ChatPath = "/api/v1/agent/chat"

// maxJSONBodyBytes bounds the request body. The largest legal payload is a
// 500-character question plus a position, so this is generous by an order of
// magnitude and still refuses a body that is obviously not a question.
const maxJSONBodyBytes = 16 * 1024

// identityProvider is the authentication boundary. auth.Handlers implements it,
// so the assistant package does not have to know how a session is verified -
// only that an identity is required and which role may use it.
type identityProvider interface {
	RequireRole(role string, next http.HandlerFunc) http.HandlerFunc
}

// Handlers serves the assistant endpoint.
type Handlers struct {
	service *Service
	auth    identityProvider
}

// NewHandlers binds the endpoint to the service and the auth middleware.
func NewHandlers(service *Service, authMiddleware identityProvider) (*Handlers, error) {
	if service == nil {
		return nil, errors.New("agent: service is required")
	}
	if authMiddleware == nil {
		return nil, errors.New("agent: auth middleware is required")
	}
	return &Handlers{service: service, auth: authMiddleware}, nil
}

// Register attaches the route. Only a user session may call it.
func (h *Handlers) Register(server interface {
	Register(pattern string, handler http.HandlerFunc)
}) {
	server.Register(ChatPath, h.withBudget(h.auth.RequireRole(auth.RoleUser, h.chat)))
}

// chatRequest is the request body.
//
// The position members are pointers because zero is a real coordinate - the
// equator and the prime meridian - so "absent" and "zero" have to be
// distinguishable, and a request that half-supplies a position must be refused
// rather than answered from a point the user never named.
type chatRequest struct {
	Message        string        `json:"message"`
	Location       *locationBody `json:"location"`
	CoordinateType string        `json:"coordinateType"`
	ChargerType    *int          `json:"chargerType"`
}

type locationBody struct {
	LatitudeE6  *int64 `json:"latitudeE6"`
	LongitudeE6 *int64 `json:"longitudeE6"`
}

// chat handles POST /api/v1/agent/chat.
func (h *Handlers) chat(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodPost) {
		return
	}

	// The caller's identity comes from the session, never from the body: a
	// client must not be able to ask the assistant about another account.
	identity, ok := auth.IdentityFromContext(r.Context())
	if !ok {
		httpapi.WriteError(w, r, http.StatusUnauthorized, httpapi.CodeUnauthorized, "authentication is required", nil)
		return
	}

	body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, maxJSONBodyBytes))
	if err != nil {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid request body", nil)
		return
	}
	// A number the caller wrote as 1.0 must not be rejected for its spelling,
	// and an integer is never silently truncated into a coordinate.
	decoder := json.NewDecoder(strings.NewReader(string(body)))
	decoder.UseNumber()
	var request chatRequest
	if err := decoder.Decode(&request); err != nil {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, "invalid request body", nil)
		return
	}

	conversation, err := h.conversation(identity, r, request)
	if err != nil {
		httpapi.WriteError(w, r, http.StatusBadRequest, httpapi.CodeInvalidArgument, err.Error(), nil)
		return
	}

	result := h.service.Chat(r.Context(), strings.TrimSpace(request.Message), conversation)

	httpapi.WriteJSON(w, http.StatusOK, httpapi.Response{
		Success: true,
		Code:    httpapi.CodeOK,
		Message: "ok",
		Data:    newChatResponseBody(result),
	})
}

// conversation validates the request and turns it into the service's context.
//
// Every message here is written for the caller: it says which field is wrong
// and what would be accepted, because these are the only failures the endpoint
// reports - everything else is answered, degraded.
func (h *Handlers) conversation(identity auth.Identity, r *http.Request, request chatRequest) (Context, error) {
	message := strings.TrimSpace(request.Message)
	if message == "" {
		return Context{}, errors.New("message must not be empty")
	}
	if len([]rune(message)) > h.service.limits.MaxMessageLength {
		return Context{}, errors.New("message is too long")
	}

	conversation := Context{
		UserID:    identity.ID,
		RequestID: httpapi.RequestID(r.Context()),
	}

	if request.Location != nil {
		if request.Location.LatitudeE6 == nil || request.Location.LongitudeE6 == nil {
			return Context{}, errors.New("location must carry both latitudeE6 and longitudeE6")
		}
		latitude := *request.Location.LatitudeE6
		longitude := *request.Location.LongitudeE6
		if latitude < minLatitudeE6 || latitude > maxLatitudeE6 ||
			longitude < minLongitudeE6 || longitude > maxLongitudeE6 {
			return Context{}, errors.New("location is outside the coordinate range")
		}
		conversation.Location = &Location{LatitudeE6: latitude, LongitudeE6: longitude}
	}

	switch request.CoordinateType {
	case "", "gcj02":
		conversation.WGS84 = false
	case "wgs84":
		// Only meaningful with a position, and harmless without one: the flag
		// describes the coordinates, and there are none.
		conversation.WGS84 = true
	default:
		return Context{}, errors.New("coordinateType must be gcj02 or wgs84")
	}

	if request.ChargerType != nil {
		if *request.ChargerType != 0 && *request.ChargerType != 1 {
			return Context{}, errors.New("chargerType must be 0 or 1")
		}
		value := *request.ChargerType
		conversation.ChargerType = &value
	}

	return conversation, nil
}

// —— response bodies ——

// The response shapes the contract publishes. They are separate from the
// service's own types so a change in how the platform stores a station cannot
// silently change what the browser receives.

type chatResponseBody struct {
	Reply    string        `json:"reply"`
	Stations []stationBody `json:"stations"`
	Pois     []poiBody     `json:"pois"`
	Route    *routeBody    `json:"route"`
	Actions  []actionBody  `json:"actions"`
	Tools    []string      `json:"tools"`
	LLMUsed  bool          `json:"llmUsed"`
	Degraded bool          `json:"degraded"`
}

type stationBody struct {
	ID                         int64    `json:"id"`
	Code                       string   `json:"code"`
	Name                       string   `json:"name"`
	Address                    string   `json:"address"`
	Status                     string   `json:"status"`
	LatitudeE6                 int64    `json:"latitudeE6"`
	LongitudeE6                int64    `json:"longitudeE6"`
	ChargerCount               int64    `json:"chargerCount"`
	IdleChargerCount           int64    `json:"idleChargerCount"`
	OperationalChargerCount    int64    `json:"operationalChargerCount"`
	FastChargerCount           int64    `json:"fastChargerCount"`
	SlowChargerCount           int64    `json:"slowChargerCount"`
	ChargerTypes               []string `json:"chargerTypes"`
	MinPriceCentPerKwh         int64    `json:"minPriceCentPerKwh"`
	ElectricityPriceCentPerKwh int64    `json:"electricityPriceCentPerKwh"`
	ServicePriceCentPerKwh     int64    `json:"servicePriceCentPerKwh"`
	// DistanceMeter is omitted when the search had no position: zero would read
	// as "right here".
	DistanceMeter *int64 `json:"distanceMeter,omitempty"`
}

type poiBody struct {
	ID            string `json:"id"`
	Name          string `json:"name"`
	Category      string `json:"category"`
	Address       string `json:"address"`
	LatitudeE6    int64  `json:"latitudeE6"`
	LongitudeE6   int64  `json:"longitudeE6"`
	DistanceMeter int64  `json:"distanceMeter"`
	Tel           string `json:"tel"`
}

type routeStepBody struct {
	Instruction    string `json:"instruction"`
	DistanceMeter  int64  `json:"distanceMeter"`
	DurationSecond int64  `json:"durationSecond"`
}

type polylinePointBody struct {
	LatitudeE6  int64 `json:"latitudeE6"`
	LongitudeE6 int64 `json:"longitudeE6"`
}

type routeBody struct {
	DestinationName        string              `json:"destinationName"`
	OriginLatitudeE6       int64               `json:"originLatitudeE6"`
	OriginLongitudeE6      int64               `json:"originLongitudeE6"`
	DestinationLatitudeE6  int64               `json:"destinationLatitudeE6"`
	DestinationLongitudeE6 int64               `json:"destinationLongitudeE6"`
	DistanceMeter          int64               `json:"distanceMeter"`
	DurationSecond         int64               `json:"durationSecond"`
	Provider               string              `json:"provider"`
	Fallback               bool                `json:"fallback"`
	Steps                  []routeStepBody     `json:"steps"`
	Polyline               []polylinePointBody `json:"polyline"`
	BrowserURL             string              `json:"browserUrl"`
}

type actionBody struct {
	Type     string `json:"type"`
	Label    string `json:"label"`
	TargetID string `json:"targetId,omitempty"`
	URL      string `json:"url,omitempty"`
}

// newChatResponseBody renders a result for the wire.
//
// The collections are never nil: a client that receives null where it expected
// a list has to special-case it, and "no stations" is a normal answer here.
func newChatResponseBody(result Result) chatResponseBody {
	body := chatResponseBody{
		Reply:    result.Reply,
		Stations: make([]stationBody, 0, len(result.Stations)),
		Pois:     make([]poiBody, 0, len(result.Pois)),
		Actions:  make([]actionBody, 0, len(result.Actions)),
		Tools:    make([]string, 0, len(result.Tools)),
		LLMUsed:  result.LLMUsed,
		Degraded: result.Degraded,
	}
	for _, station := range result.Stations {
		item := stationBody{
			ID:                         station.ID,
			Code:                       station.Code,
			Name:                       station.Name,
			Address:                    station.Address,
			Status:                     station.Status,
			LatitudeE6:                 station.LatitudeE6,
			LongitudeE6:                station.LongitudeE6,
			ChargerCount:               station.ChargerCount,
			IdleChargerCount:           station.IdleChargerCount,
			OperationalChargerCount:    station.OperationalChargerCount,
			FastChargerCount:           station.FastChargerCount,
			SlowChargerCount:           station.SlowChargerCount,
			ChargerTypes:               station.ChargerTypes,
			MinPriceCentPerKwh:         station.TotalPriceCentPerKwh,
			ElectricityPriceCentPerKwh: station.ElectricityPriceCentPerKwh,
			ServicePriceCentPerKwh:     station.ServicePriceCentPerKwh,
		}
		if item.ChargerTypes == nil {
			item.ChargerTypes = []string{}
		}
		if station.HasDistance {
			distance := station.DistanceMeter
			item.DistanceMeter = &distance
		}
		body.Stations = append(body.Stations, item)
	}
	for _, poi := range result.Pois {
		body.Pois = append(body.Pois, poiBody{
			ID:            poi.ID,
			Name:          poi.Name,
			Category:      poi.Category,
			Address:       poi.Address,
			LatitudeE6:    poi.LatitudeE6,
			LongitudeE6:   poi.LongitudeE6,
			DistanceMeter: poi.DistanceMeter,
			Tel:           poi.Tel,
		})
	}
	if result.Route != nil {
		route := routeBody{
			DestinationName:        result.Route.DestinationName,
			OriginLatitudeE6:       result.Route.Origin.LatitudeE6,
			OriginLongitudeE6:      result.Route.Origin.LongitudeE6,
			DestinationLatitudeE6:  result.Route.Destination.LatitudeE6,
			DestinationLongitudeE6: result.Route.Destination.LongitudeE6,
			DistanceMeter:          result.Route.DistanceMeter,
			DurationSecond:         result.Route.DurationSecond,
			Provider:               result.Route.Provider,
			Fallback:               result.Route.Fallback,
			Steps:                  make([]routeStepBody, 0, len(result.Route.Steps)),
			Polyline:               make([]polylinePointBody, 0, len(result.Route.Polyline)),
			BrowserURL:             result.Route.BrowserURL,
		}
		for _, step := range result.Route.Steps {
			route.Steps = append(route.Steps, routeStepBody{
				Instruction:    step.Instruction,
				DistanceMeter:  step.DistanceMeter,
				DurationSecond: step.DurationSecond,
			})
		}
		for _, point := range result.Route.Polyline {
			route.Polyline = append(route.Polyline, polylinePointBody{
				LatitudeE6:  point.LatitudeE6,
				LongitudeE6: point.LongitudeE6,
			})
		}
		body.Route = &route
	}
	for _, action := range result.Actions {
		body.Actions = append(body.Actions, actionBody{
			Type:     action.Type,
			Label:    action.Label,
			TargetID: action.TargetID,
			URL:      action.URL,
		})
	}
	body.Tools = append(body.Tools, result.Tools...)
	return body
}

// requireMethod answers a wrong method the way every other module does.
func requireMethod(w http.ResponseWriter, r *http.Request, method string) bool {
	if r.Method == method {
		return true
	}
	w.Header().Set("Allow", method)
	httpapi.WriteError(w, r, http.StatusMethodNotAllowed, httpapi.CodeMethodNotAllowed, "method not allowed", nil)
	return false
}
