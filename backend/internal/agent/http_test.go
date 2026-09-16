package agent

import (
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/heguangV/charging-station-platform/backend/internal/auth"
	"github.com/heguangV/charging-station-platform/backend/internal/config"
	"github.com/heguangV/charging-station-platform/backend/internal/httpapi"
	"github.com/heguangV/charging-station-platform/backend/internal/station"
)

// The endpoint's own rules: who may call it, what it refuses before doing any
// work, and the exact shape a client receives. The orchestration is covered
// elsewhere; what is asserted here is the boundary.

// testAuth stands in for auth.Handlers. It records which role the endpoint
// asked for, which is how the "a user session, not an admin one" rule is
// asserted rather than assumed.
type testAuth struct {
	identity auth.Identity
	reject   bool
	roles    []string
}

func (a *testAuth) RequireRole(role string, next http.HandlerFunc) http.HandlerFunc {
	a.roles = append(a.roles, role)
	return func(w http.ResponseWriter, r *http.Request) {
		if a.reject {
			httpapi.WriteError(w, r, http.StatusUnauthorized, httpapi.CodeUnauthorized, "authentication is required", nil)
			return
		}
		next(w, r.WithContext(auth.WithIdentity(r.Context(), a.identity)))
	}
}

func newEndpoint(t *testing.T, stations StationDirectory, options ...func(*Config)) (*testAuth, http.Handler) {
	t.Helper()
	cfg := Config{Stations: stations, Logger: slog.New(slog.NewTextHandler(io.Discard, nil))}
	for _, option := range options {
		option(&cfg)
	}
	service, err := NewService(cfg)
	if err != nil {
		t.Fatalf("NewService() error = %v", err)
	}
	authMiddleware := &testAuth{identity: auth.Identity{ID: 42, Role: auth.RoleUser, DisplayName: "车主"}}
	handlers, err := NewHandlers(service, authMiddleware)
	if err != nil {
		t.Fatalf("NewHandlers() error = %v", err)
	}
	server := httpapi.NewServer(config.Config{RequestIDHeader: "X-Request-ID"},
		slog.New(slog.NewTextHandler(io.Discard, nil)))
	handlers.Register(server)
	server.SetReady(true)
	return authMiddleware, server.Handler()
}

func postChat(t *testing.T, handler http.Handler, body string) *httptest.ResponseRecorder {
	t.Helper()
	request := httptest.NewRequest(http.MethodPost, ChatPath, strings.NewReader(body))
	request.Header.Set("Content-Type", "application/json")
	recorder := httptest.NewRecorder()
	handler.ServeHTTP(recorder, request)
	return recorder
}

// decodeEnvelope splits the shared envelope from the assistant payload.
func decodeEnvelope(t *testing.T, recorder *httptest.ResponseRecorder) (httpapi.Response, map[string]any) {
	t.Helper()
	var raw struct {
		Success bool            `json:"success"`
		Code    int             `json:"code"`
		Message string          `json:"message"`
		Data    json.RawMessage `json:"data"`
	}
	if err := json.Unmarshal(recorder.Body.Bytes(), &raw); err != nil {
		t.Fatalf("the response is not an envelope: %s", recorder.Body.String())
	}
	payload := map[string]any{}
	if len(raw.Data) > 0 {
		if err := json.Unmarshal(raw.Data, &payload); err != nil {
			t.Fatalf("the payload is not an object: %s", raw.Data)
		}
	}
	return httpapi.Response{Success: raw.Success, Code: raw.Code, Message: raw.Message}, payload
}

// —— authentication ——

// Only a user session may reach the assistant: the questions it answers are the
// C-end user's, and an anonymous caller has no account context at all.
func TestChatEndpointRequiresAUserSession(t *testing.T) {
	stations := &fakeStations{}
	authMiddleware, handler := newEndpoint(t, stations)

	// The middleware is what refuses an unauthenticated call; the endpoint's own
	// obligation is to ask for a user session and to do no work when it is
	// refused. Both are asserted, because the second is the one that would
	// quietly leak a search to an anonymous caller.
	recorder := postChat(t, handler, `{"message":"附近哪有充电站"}`)
	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200 for an authenticated call", recorder.Code)
	}
	if len(authMiddleware.roles) != 1 || authMiddleware.roles[0] != auth.RoleUser {
		t.Fatalf("roles = %v, want the endpoint to ask for a user session", authMiddleware.roles)
	}

	authMiddleware.reject = true
	stations.searches = nil
	recorder = postChat(t, handler, `{"message":"附近哪有充电站"}`)
	if recorder.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want 401 when the session is refused", recorder.Code)
	}
	if len(stations.searches) != 0 {
		t.Fatal("a refused session must not reach the domain")
	}
}

// —— method and body ——

func TestChatEndpointRejectsOtherMethods(t *testing.T) {
	_, handler := newEndpoint(t, &fakeStations{})
	request := httptest.NewRequest(http.MethodGet, ChatPath, nil)
	recorder := httptest.NewRecorder()
	handler.ServeHTTP(recorder, request)

	if recorder.Code != http.StatusMethodNotAllowed {
		t.Fatalf("status = %d, want 405", recorder.Code)
	}
	if allow := recorder.Header().Get("Allow"); allow != http.MethodPost {
		t.Fatalf("Allow = %q, want POST", allow)
	}
}

func TestChatEndpointRejectsABodyThatIsNotJSON(t *testing.T) {
	_, handler := newEndpoint(t, &fakeStations{})
	recorder := postChat(t, handler, `{not json`)
	if recorder.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", recorder.Code)
	}
}

// The failures below are the only ones the endpoint reports. Everything else is
// answered, degraded - so each of these has to be refused before any work is
// done, with a message that says which field is wrong.
func TestChatEndpointRefusesAnInvalidRequest(t *testing.T) {
	cases := map[string]string{
		"empty message":          `{"message":""}`,
		"whitespace message":     `{"message":"   "}`,
		"missing message":        `{}`,
		"message too long":       `{"message":"` + strings.Repeat("充", 501) + `"}`,
		"half a location":        `{"message":"附近哪有充电站","location":{"latitudeE6":39977680}}`,
		"latitude out of range":  `{"message":"附近哪有充电站","location":{"latitudeE6":91000000,"longitudeE6":116316417}}`,
		"longitude out of range": `{"message":"附近哪有充电站","location":{"latitudeE6":39977680,"longitudeE6":181000000}}`,
		"unknown coordinate":     `{"message":"附近哪有充电站","location":{"latitudeE6":39977680,"longitudeE6":116316417},"coordinateType":"bd09"}`,
		"unknown charger type":   `{"message":"附近哪有充电站","chargerType":2}`,
	}
	for name, body := range cases {
		t.Run(name, func(t *testing.T) {
			stations := &fakeStations{}
			_, handler := newEndpoint(t, stations)

			recorder := postChat(t, handler, body)

			if recorder.Code != http.StatusBadRequest {
				t.Fatalf("status = %d, want 400 (%s)", recorder.Code, recorder.Body.String())
			}
			if len(stations.searches) != 0 {
				t.Fatal("a refused request must not reach the domain")
			}
		})
	}
}

// A message of exactly the maximum length is accepted: the bound is a bound,
// not an approximation.
func TestChatEndpointAcceptsAMessageAtTheLimit(t *testing.T) {
	_, handler := newEndpoint(t, &fakeStations{})
	recorder := postChat(t, handler, `{"message":"`+strings.Repeat("充", 500)+`"}`)
	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200 (%s)", recorder.Code, recorder.Body.String())
	}
}

// —— the response contract ——

// The collections are lists even when empty. A client that receives null where
// it expects an array has to special-case it, and "no stations" is a normal
// answer.
func TestChatEndpointReturnsListsEvenWhenEmpty(t *testing.T) {
	_, handler := newEndpoint(t, &fakeStations{})

	recorder := postChat(t, handler, `{"message":"附近哪有充电站"}`)
	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", recorder.Code)
	}
	envelope, payload := decodeEnvelope(t, recorder)
	if !envelope.Success || envelope.Code != 0 {
		t.Fatalf("envelope = %+v, want a success", envelope)
	}
	for _, key := range []string{"stations", "pois", "actions", "tools"} {
		value, present := payload[key]
		if !present {
			t.Fatalf("the payload is missing %q", key)
		}
		if _, isList := value.([]any); !isList {
			t.Fatalf("%s = %#v, want a list", key, value)
		}
	}
	route, present := payload["route"]
	if !present {
		t.Fatal("the payload must carry a route member")
	}
	if route != nil {
		t.Fatalf("route = %#v, want null when none was planned", route)
	}
	for _, key := range []string{"reply", "llmUsed", "degraded"} {
		if _, present := payload[key]; !present {
			t.Fatalf("the payload is missing %q", key)
		}
	}
	if payload["reply"] == "" {
		t.Fatal("the reply must never be empty")
	}
}

// The station payload is the contract's own shape plus the price split and the
// charger mix the recommendation card renders.
func TestChatEndpointSerializesAStationForTheClient(t *testing.T) {
	_, handler := newEndpoint(t, &fakeStations{summaries: []station.Summary{sampleStation(9, "中关村站")}})

	recorder := postChat(t, handler,
		`{"message":"附近哪有充电站","location":{"latitudeE6":39977680,"longitudeE6":116316417},"coordinateType":"gcj02"}`)
	_, payload := decodeEnvelope(t, recorder)

	stations, _ := payload["stations"].([]any)
	if len(stations) != 1 {
		t.Fatalf("stations = %#v, want one", payload["stations"])
	}
	first, _ := stations[0].(map[string]any)
	if first["name"] != "中关村站" || first["id"] != float64(9) {
		t.Fatalf("station = %#v", first)
	}
	// The split has to add up: a card may show the total or its parts.
	total, _ := first["minPriceCentPerKwh"].(float64)
	electricity, _ := first["electricityPriceCentPerKwh"].(float64)
	service, _ := first["servicePriceCentPerKwh"].(float64)
	if total != electricity+service {
		t.Fatalf("the price split %v + %v does not add up to %v", electricity, service, total)
	}
	if first["distanceMeter"] != float64(2300) {
		t.Fatalf("distanceMeter = %#v, want the search distance", first["distanceMeter"])
	}
	// The charger mix is a list, not null, so a card can iterate it directly.
	if _, isList := first["chargerTypes"].([]any); !isList {
		t.Fatalf("chargerTypes = %#v, want a list", first["chargerTypes"])
	}
}

// A station found by keyword has no distance, and must not report zero: that
// reads as "right here".
func TestChatEndpointOmitsAMeaninglessDistance(t *testing.T) {
	found := sampleStation(9, "中关村站")
	found.HasDistance = false
	found.DistanceMeter = 0
	_, handler := newEndpoint(t, &fakeStations{summaries: []station.Summary{found}})

	recorder := postChat(t, handler, `{"message":"中关村充电站在哪"}`)
	_, payload := decodeEnvelope(t, recorder)

	stations, _ := payload["stations"].([]any)
	first, _ := stations[0].(map[string]any)
	if _, present := first["distanceMeter"]; present {
		t.Fatalf("distanceMeter = %#v, want it omitted", first["distanceMeter"])
	}
}

// The route is an object when one was planned, with the fallback marked, and
// the navigation link is what the button opens.
func TestChatEndpointSerializesARoute(t *testing.T) {
	_, handler := newEndpoint(t, &fakeStations{summaries: []station.Summary{sampleStation(9, "中关村站")}},
		func(cfg *Config) {
			cfg.Routes = &fakeRoutes{route: Route{
				DestinationName: "中关村站",
				Origin:          Location{LatitudeE6: 39977680, LongitudeE6: 116316417},
				Destination:     Location{LatitudeE6: 39980000, LongitudeE6: 116320000},
				DistanceMeter:   2300, DurationSecond: 480,
				Provider: ProviderLocalFallback, Fallback: true,
				Steps:      []RouteStep{{Instruction: "向东行驶", DistanceMeter: 300, DurationSecond: 60}},
				Polyline:   []RoutePoint{{LatitudeE6: 39977680, LongitudeE6: 116316417}},
				BrowserURL: "https://apis.map.qq.com/uri/v1/routeplan?type=drive",
			}}
		})

	recorder := postChat(t, handler, `{"message":"导航到最近的充电站","location":{"latitudeE6":39977680,"longitudeE6":116316417}}`)
	_, payload := decodeEnvelope(t, recorder)

	route, isObject := payload["route"].(map[string]any)
	if !isObject {
		t.Fatalf("route = %#v, want an object", payload["route"])
	}
	if route["fallback"] != true {
		t.Fatalf("fallback = %#v, want the estimate to be marked", route["fallback"])
	}
	if route["browserUrl"] == "" {
		t.Fatal("the navigation link must be present so the button has somewhere to go")
	}
	if payload["degraded"] != true {
		t.Fatal("a fallback route must be reported as degraded")
	}
	if _, isList := route["steps"].([]any); !isList {
		t.Fatalf("steps = %#v, want a list", route["steps"])
	}
}

// The person asking is taken from the session, never from the body: a client
// must not be able to ask about another account.
func TestChatEndpointTakesTheUserFromTheSession(t *testing.T) {
	stations := &fakeStations{}
	authMiddleware, handler := newEndpoint(t, stations)
	authMiddleware.identity = auth.Identity{ID: 777, Role: auth.RoleUser}

	recorder := postChat(t, handler, `{"message":"附近哪有充电站","userId":1,"location":{"latitudeE6":39977680,"longitudeE6":116316417}}`)
	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", recorder.Code)
	}
	// The body's userId is ignored rather than trusted; the search itself is
	// user-scoped by the session the middleware resolved.
	if len(stations.searches) != 1 {
		t.Fatalf("searches = %d, want 1", len(stations.searches))
	}
	if !stations.searches[0].HasLocation {
		t.Fatal("the position from the body must still be used")
	}
}
