package geo

import (
	"bytes"
	"context"
	"errors"
	"io"
	"log/slog"
	"net"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"
	"time"

	"github.com/heguangV/charging-station-platform/backend/internal/agent"
)

// The map adapter is the only place the platform speaks a provider's wire
// format, so these tests pin that format: a rename on either side has to fail
// here rather than in a driver's answer.

// stubProvider is a stand-in for the WebService. It records the last request so
// the query the platform builds can be asserted, not just the answer.
type stubProvider struct {
	status  int
	body    string
	lastURL *url.URL
	calls   int
}

func (s *stubProvider) handler() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		s.calls++
		s.lastURL = r.URL
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(s.status)
		_, _ = io.WriteString(w, s.body)
	}
}

func newStubClient(t *testing.T, stub *stubProvider) *Client {
	t.Helper()
	server := httptest.NewServer(stub.handler())
	t.Cleanup(server.Close)
	return NewClient(ClientConfig{
		ServerKey: "test-server-key",
		BaseURL:   server.URL,
		Timeout:   2 * time.Second,
		Logger:    slog.New(slog.NewTextHandler(io.Discard, nil)),
	})
}

// —— the local estimates ——

func TestHaversineMeterMatchesAKnownDistance(t *testing.T) {
	// Tiananmen to the Bird's Nest is about 9.2 km.
	got := HaversineMeter(39908498, 116391137, 39992900, 116390600)
	if got < 9000 || got > 9400 {
		t.Fatalf("distance = %d, want roughly 9200 m", got)
	}
}

func TestHaversineMeterIsZeroForOnePoint(t *testing.T) {
	if got := HaversineMeter(39977680, 116316417, 39977680, 116316417); got != 0 {
		t.Fatalf("distance = %d, want 0", got)
	}
}

// The provider wants six decimals, and both a negative coordinate and one with
// leading zeros in the fraction have to survive the formatting.
func TestDecimal6KeepsSignAndPrecision(t *testing.T) {
	cases := map[int64]string{
		39977680:  "39.977680",
		-1234567:  "-1.234567",
		50000:     "0.050000",
		0:         "0.000000",
		116316417: "116.316417",
		-500000:   "-0.500000",
	}
	for input, want := range cases {
		if got := decimal6(input); got != want {
			t.Fatalf("decimal6(%d) = %q, want %q", input, got, want)
		}
	}
}

func TestCoordinateIsLatitudeFirst(t *testing.T) {
	if got := coordinate(39977680, 116316417); got != "39.977680,116.316417" {
		t.Fatalf("coordinate() = %q", got)
	}
}

// —— the place search ——

// The query the provider receives is the contract: the boundary syntax, the
// ordering and the page size all have to be what the provider expects.
func TestSearchPoisBuildsTheProviderQuery(t *testing.T) {
	stub := &stubProvider{status: http.StatusOK, body: `{"status":0,"data":[]}`}
	client := newStubClient(t, stub)

	_, err := client.SearchPois(context.Background(), agent.PoiQuery{
		Location:    agent.Location{LatitudeE6: 39977680, LongitudeE6: 116316417},
		Category:    "咖啡",
		RadiusMeter: 1500,
		Limit:       4,
	})
	if err != nil {
		t.Fatalf("SearchPois() error = %v", err)
	}
	query := stub.lastURL.Query()
	if stub.lastURL.Path != placeSearchPath {
		t.Fatalf("path = %q, want %q", stub.lastURL.Path, placeSearchPath)
	}
	// The category is a business concept; the provider searches for a keyword.
	if query.Get("keyword") != "咖啡厅" {
		t.Fatalf("keyword = %q, want the provider's own term", query.Get("keyword"))
	}
	if query.Get("boundary") != "nearby(39.977680,116.316417,1500)" {
		t.Fatalf("boundary = %q", query.Get("boundary"))
	}
	if query.Get("page_size") != "4" || query.Get("page_index") != "1" {
		t.Fatalf("paging = %q/%q", query.Get("page_size"), query.Get("page_index"))
	}
	if query.Get("orderby") != "_distance" {
		t.Fatalf("orderby = %q", query.Get("orderby"))
	}
	if query.Get("key") != "test-server-key" {
		t.Fatalf("key = %q, want the configured server key", query.Get("key"))
	}
}

func TestSearchPoisParsesTheProvidersAnswer(t *testing.T) {
	stub := &stubProvider{status: http.StatusOK, body: `{"status":0,"data":[
        {"id":"POI-1","title":"星巴克","category":"咖啡厅","address":"中关村大街 1 号","tel":"010-1",
         "location":{"lat":39.978100,"lng":116.316000},"_distance":210},
        {"id":"POI-2","title":"便利店","category":"便利店","address":"","tel_1":"010-2",
         "location":{"lat":39.979000,"lng":116.317000},"_distance":0}
    ]}`}
	client := newStubClient(t, stub)

	pois, err := client.SearchPois(context.Background(), agent.PoiQuery{
		Location: agent.Location{LatitudeE6: 39977680, LongitudeE6: 116316417},
		Limit:    5,
	})
	if err != nil {
		t.Fatalf("SearchPois() error = %v", err)
	}
	if len(pois) != 2 {
		t.Fatalf("pois = %d, want 2", len(pois))
	}
	if pois[0].Name != "星巴克" || pois[0].DistanceMeter != 210 {
		t.Fatalf("first poi = %+v", pois[0])
	}
	if pois[0].LatitudeE6 != 39978100 || pois[0].LongitudeE6 != 116316000 {
		t.Fatalf("first poi coordinates = %d/%d", pois[0].LatitudeE6, pois[0].LongitudeE6)
	}
	// The provider does not always report a distance; showing zero would read as
	// "right here", so it is computed instead.
	if pois[1].DistanceMeter <= 0 {
		t.Fatalf("second poi distance = %d, want a computed estimate", pois[1].DistanceMeter)
	}
	// The alternative telephone field is used when the primary one is absent.
	if pois[1].Tel != "010-2" {
		t.Fatalf("second poi tel = %q", pois[1].Tel)
	}
}

// A place with no coordinates cannot be shown or routed to, and a place with no
// name cannot be read: both are dropped rather than shown as a dead card.
func TestSearchPoisDropsUnusableEntries(t *testing.T) {
	stub := &stubProvider{status: http.StatusOK, body: `{"status":0,"data":[
        {"id":"A","title":"没有坐标","location":{}},
        {"id":"B","title":"","location":{"lat":39.9781,"lng":116.316}},
        {"id":"C","title":"原点","location":{"lat":0,"lng":0}},
        {"id":"D","title":"可用","location":{"lat":39.9781,"lng":116.316},"_distance":50}
    ]}`}
	client := newStubClient(t, stub)

	pois, err := client.SearchPois(context.Background(), agent.PoiQuery{
		Location: agent.Location{LatitudeE6: 39977680, LongitudeE6: 116316417},
		Limit:    5,
	})
	if err != nil {
		t.Fatalf("SearchPois() error = %v", err)
	}
	if len(pois) != 1 || pois[0].ID != "D" {
		t.Fatalf("pois = %+v, want only the usable one", pois)
	}
}

// An empty but successful answer is an empty list, not a failure: "there is
// nothing nearby" is a real answer.
func TestSearchPoisTreatsAnEmptyAnswerAsSuccess(t *testing.T) {
	stub := &stubProvider{status: http.StatusOK, body: `{"status":0,"data":[]}`}
	client := newStubClient(t, stub)

	pois, err := client.SearchPois(context.Background(), agent.PoiQuery{
		Location: agent.Location{LatitudeE6: 39977680, LongitudeE6: 116316417},
		Limit:    5,
	})
	if err != nil {
		t.Fatalf("SearchPois() error = %v, want an empty success", err)
	}
	if len(pois) != 0 {
		t.Fatalf("pois = %+v, want none", pois)
	}
}

// The provider reports a business failure inside a 200 response, so the status
// code alone is not enough to tell success from failure.
func TestSearchPoisReportsAProviderRefusal(t *testing.T) {
	stub := &stubProvider{status: http.StatusOK, body: `{"status":120,"message":"此key每日调用量已达到上限"}`}
	client := newStubClient(t, stub)

	_, err := client.SearchPois(context.Background(), agent.PoiQuery{
		Location: agent.Location{LatitudeE6: 39977680, LongitudeE6: 116316417},
		Limit:    5,
	})
	if err == nil {
		t.Fatal("expected a provider business failure to be reported")
	}
}

// Without a key the provider must not be called at all: an unauthenticated call
// would be answered with an error that reads like an outage.
func TestSearchPoisRefusesWithoutAKey(t *testing.T) {
	stub := &stubProvider{status: http.StatusOK, body: `{"status":0,"data":[]}`}
	server := httptest.NewServer(stub.handler())
	defer server.Close()
	client := NewClient(ClientConfig{BaseURL: server.URL})

	if client.Available() {
		t.Fatal("a client without a key must report itself unavailable")
	}
	if _, err := client.SearchPois(context.Background(), agent.PoiQuery{
		Location: agent.Location{LatitudeE6: 39977680, LongitudeE6: 116316417},
	}); err == nil {
		t.Fatal("expected a call without a key to be refused")
	}
	if stub.calls != 0 {
		t.Fatalf("calls = %d, want none", stub.calls)
	}
}

// —— the route ——

// A route the provider could plan is used as-is, with the provider's minutes
// converted to the seconds everything else speaks.
func TestPlanRouteUsesTheProvidersRoute(t *testing.T) {
	// The encoding is relative: the first pair is absolute, later pairs are
	// offsets in 1e-6 degrees from the pair two places before.
	body := `{"status":0,"result":{"routes":[{
        "distance":2300,"duration":8,
        "polyline":[39.977680,116.316417,0,3000,1000,0],
        "steps":[{"instruction":"向东行驶","distance":300,"duration":1}]
    }]}}`
	stub := &stubProvider{status: http.StatusOK, body: body}
	client := newStubClient(t, stub)

	route, err := client.PlanRoute(context.Background(), agent.RouteRequest{
		Origin:          agent.Location{LatitudeE6: 39977680, LongitudeE6: 116316417},
		Destination:     agent.Location{LatitudeE6: 39980000, LongitudeE6: 116320000},
		Mode:            agent.TravelDriving,
		DestinationName: "中关村站",
	})
	if err != nil {
		t.Fatalf("PlanRoute() error = %v", err)
	}
	if route.Fallback {
		t.Fatal("a planned route must not be marked as a fallback")
	}
	if route.Provider != agent.ProviderTencentMap {
		t.Fatalf("provider = %q", route.Provider)
	}
	if route.DistanceMeter != 2300 {
		t.Fatalf("distance = %d", route.DistanceMeter)
	}
	// Eight minutes, not eight seconds.
	if route.DurationSecond != 480 {
		t.Fatalf("duration = %d, want 480 seconds", route.DurationSecond)
	}
	if len(route.Polyline) != 3 {
		t.Fatalf("polyline = %+v, want three points", route.Polyline)
	}
	if route.Polyline[1].LatitudeE6 != 39977680 || route.Polyline[1].LongitudeE6 != 116319417 {
		t.Fatalf("second point = %+v, want the decoded offset", route.Polyline[1])
	}
	if len(route.Steps) != 1 || route.Steps[0].Instruction != "向东行驶" {
		t.Fatalf("steps = %+v", route.Steps)
	}
	if route.BrowserURL == "" {
		t.Fatal("a route must carry a navigation link")
	}
	if stub.lastURL.Path != directionPathPrefix+"driving" {
		t.Fatalf("path = %q", stub.lastURL.Path)
	}
}

// A provider that is down, or a key that is absent, produces the straight-line
// estimate with the fallback marked - never an error.
func TestPlanRouteFallsBackWhenTheProviderCannotAnswer(t *testing.T) {
	cases := map[string]*Client{
		"provider refuses": newStubClient(t, &stubProvider{status: http.StatusOK, body: `{"status":120,"message":"limit"}`}),
		"no key":           NewClient(ClientConfig{BaseURL: "https://apis.map.qq.com"}),
	}
	for name, client := range cases {
		t.Run(name, func(t *testing.T) {
			route, err := client.PlanRoute(context.Background(), agent.RouteRequest{
				Origin:      agent.Location{LatitudeE6: 39977680, LongitudeE6: 116316417},
				Destination: agent.Location{LatitudeE6: 39980000, LongitudeE6: 116320000},
				Mode:        agent.TravelDriving,
			})
			if err != nil {
				t.Fatalf("PlanRoute() error = %v, want a usable estimate", err)
			}
			if !route.Fallback || route.Provider != agent.ProviderLocalFallback {
				t.Fatalf("route = %+v, want the fallback marked", route)
			}
			if route.DistanceMeter <= 0 {
				t.Fatalf("distance = %d, want the straight-line estimate", route.DistanceMeter)
			}
			// A straight line has no speed, so it has no duration either.
			if route.DurationSecond != 0 {
				t.Fatalf("duration = %d, want 0 for an estimate", route.DurationSecond)
			}
			if len(route.Polyline) != 2 {
				t.Fatalf("polyline = %+v, want the two endpoints", route.Polyline)
			}
			if route.BrowserURL == "" {
				t.Fatal("even an estimate must offer a navigation link")
			}
		})
	}
}

// A provider that answers with a status of zero and a route that goes nowhere
// has not planned anything: distance one metre or a path of one repeated point
// is a degenerate answer, and the platform's own estimate is better.
func TestPlanRouteRejectsADegenerateRoute(t *testing.T) {
	cases := map[string]string{
		"distance too small": `{"status":0,"result":{"routes":[{"distance":1,"duration":8,"polyline":[39.97768,116.316417,0,0.001],"steps":[]}]}}`,
		"no duration":        `{"status":0,"result":{"routes":[{"distance":2300,"duration":0,"polyline":[39.97768,116.316417,0,0.001],"steps":[]}]}}`,
		"one repeated point": `{"status":0,"result":{"routes":[{"distance":2300,"duration":8,"polyline":[39.97768,116.316417,0,0],"steps":[]}]}}`,
		"no routes":          `{"status":0,"result":{"routes":[]}}`,
	}
	for name, body := range cases {
		t.Run(name, func(t *testing.T) {
			client := newStubClient(t, &stubProvider{status: http.StatusOK, body: body})
			route, err := client.PlanRoute(context.Background(), agent.RouteRequest{
				Origin:      agent.Location{LatitudeE6: 39977680, LongitudeE6: 116316417},
				Destination: agent.Location{LatitudeE6: 39980000, LongitudeE6: 116320000},
				Mode:        agent.TravelDriving,
			})
			if err != nil {
				t.Fatalf("PlanRoute() error = %v", err)
			}
			if !route.Fallback {
				t.Fatalf("route = %+v, want the degenerate answer replaced by the estimate", route)
			}
		})
	}
}

// A route from a point to itself is not a route. Asking the provider would
// spend a request to be told so.
func TestPlanRouteDoesNotAskForADegenerateJourney(t *testing.T) {
	stub := &stubProvider{status: http.StatusOK, body: `{"status":0,"result":{"routes":[]}}`}
	client := newStubClient(t, stub)

	route, err := client.PlanRoute(context.Background(), agent.RouteRequest{
		Origin:      agent.Location{LatitudeE6: 39977680, LongitudeE6: 116316417},
		Destination: agent.Location{LatitudeE6: 39977680, LongitudeE6: 116316417},
		Mode:        agent.TravelDriving,
	})
	if err != nil {
		t.Fatalf("PlanRoute() error = %v", err)
	}
	if stub.calls != 0 {
		t.Fatalf("calls = %d, want none for a zero-length journey", stub.calls)
	}
	// The platform still answers: a journey of no length is zero metres, not an
	// error, and the caller can say so. What must not happen is a wasted request.
	if route.DistanceMeter != 0 || route.DurationSecond != 0 {
		t.Fatalf("route = %+v, want a zero-length answer", route)
	}
	if route.BrowserURL == "" {
		t.Fatal("even a zero-length journey carries a link, so the button is not dead")
	}
}

// A position outside the world is refused rather than sent: the provider would
// answer with an error that reads like an outage.
func TestPlanRouteRefusesImpossibleCoordinates(t *testing.T) {
	stub := &stubProvider{status: http.StatusOK, body: `{"status":0,"result":{}}`}
	client := newStubClient(t, stub)

	if _, err := client.PlanRoute(context.Background(), agent.RouteRequest{
		Origin:      agent.Location{LatitudeE6: 199_000_000, LongitudeE6: 116316417},
		Destination: agent.Location{LatitudeE6: 39980000, LongitudeE6: 116320000},
	}); err == nil {
		t.Fatal("expected an impossible origin to be refused")
	}
	if stub.calls != 0 {
		t.Fatalf("calls = %d, want none", stub.calls)
	}
}

// —— the navigation link ——

// The link is the platform's hand-off to the provider's own page, so it has to
// be exactly what that page expects - and it must carry no credential.
func TestBrowserRouteURLIsTheProvidersLink(t *testing.T) {
	got := BrowserRouteURL(
		agent.Location{LatitudeE6: 39977680, LongitudeE6: 116316417},
		agent.Location{LatitudeE6: 39980000, LongitudeE6: 116320000},
		"中关村充电站", agent.TravelDriving)

	if !strings.HasPrefix(got, "https://apis.map.qq.com/uri/v1/routeplan?type=drive") {
		t.Fatalf("url = %q", got)
	}
	if !strings.Contains(got, "&fromcoord=39.977680,116.316417") {
		t.Fatalf("url = %q, want the origin coordinates unescaped", got)
	}
	if !strings.Contains(got, "&tocoord=39.980000,116.320000") {
		t.Fatalf("url = %q, want the destination coordinates", got)
	}
	if !strings.Contains(got, "&referer=NCS") {
		t.Fatalf("url = %q, want the referer", got)
	}
	// A Chinese name is percent-encoded, and the credential is nowhere in it.
	if !strings.Contains(got, "&to=%E4%B8%AD%E5%85%B3%E6%9D%91") {
		t.Fatalf("url = %q, want the destination name encoded", got)
	}
	if strings.Contains(got, "key=") || strings.Contains(got, "test-server-key") {
		t.Fatalf("the navigation link must never carry a credential: %q", got)
	}
}

func TestBrowserRouteURLUsesTheTravelMode(t *testing.T) {
	origin := agent.Location{LatitudeE6: 39977680, LongitudeE6: 116316417}
	destination := agent.Location{LatitudeE6: 39980000, LongitudeE6: 116320000}
	cases := map[agent.TravelMode]string{
		agent.TravelDriving: "type=drive",
		agent.TravelWalking: "type=walk",
		agent.TravelTransit: "type=bus",
	}
	for mode, want := range cases {
		got := BrowserRouteURL(origin, destination, "站", mode)
		if !strings.Contains(got, want) {
			t.Fatalf("mode %s: url = %q, want %q", mode, got, want)
		}
	}
}

// —— the datum conversion ——

func TestToGCJ02ReturnsTheTranslatedPoint(t *testing.T) {
	stub := &stubProvider{status: http.StatusOK, body: `{"status":0,"locations":[{"lat":39.982100,"lng":116.321000}]}`}
	client := newStubClient(t, stub)

	converted, err := client.ToGCJ02(agent.Location{LatitudeE6: 39977680, LongitudeE6: 116316417})
	if err != nil {
		t.Fatalf("ToGCJ02() error = %v", err)
	}
	if converted.LatitudeE6 != 39982100 || converted.LongitudeE6 != 116321000 {
		t.Fatalf("converted = %+v", converted)
	}
	if stub.lastURL.Path != translatePath {
		t.Fatalf("path = %q", stub.lastURL.Path)
	}
	if got := stub.lastURL.Query().Get("locations"); got != "39.977680,116.316417" {
		t.Fatalf("locations = %q", got)
	}
	if got := stub.lastURL.Query().Get("type"); got != "1" {
		t.Fatalf("type = %q, want the GPS conversion code", got)
	}
}

// A provider that answers with something that is not exactly one valid point
// has not performed the conversion, and returning the input would be worse than
// failing: the caller would search from a position it believes was corrected.
func TestToGCJ02RefusesAnUnusableAnswer(t *testing.T) {
	cases := map[string]string{
		"no locations":   `{"status":0,"locations":[]}`,
		"two locations":  `{"status":0,"locations":[{"lat":39.9,"lng":116.3},{"lat":39.9,"lng":116.3}]}`,
		"missing values": `{"status":0,"locations":[{"lat":39.9}]}`,
		"impossible":     `{"status":0,"locations":[{"lat":91,"lng":116.3}]}`,
		"refused":        `{"status":120,"message":"limit"}`,
	}
	for name, body := range cases {
		t.Run(name, func(t *testing.T) {
			client := newStubClient(t, &stubProvider{status: http.StatusOK, body: body})
			if _, err := client.ToGCJ02(agent.Location{LatitudeE6: 39977680, LongitudeE6: 116316417}); err == nil {
				t.Fatal("expected the conversion to be refused")
			}
		})
	}
}

// The key must never appear in a URL the platform logs or hands out, so the
// only place it is allowed is the provider request itself.
func TestProviderCallsCarryTheKeyOnlyToTheProvider(t *testing.T) {
	stub := &stubProvider{status: http.StatusOK, body: `{"status":0,"data":[]}`}
	client := newStubClient(t, stub)

	_, _ = client.SearchPois(context.Background(), agent.PoiQuery{
		Location: agent.Location{LatitudeE6: 39977680, LongitudeE6: 116316417},
		Limit:    1,
	})

	// A malformed body is handled by the caller; what matters is that no URL the
	// platform builds for the browser ever carries the key.
	if strings.Contains(BrowserRouteURL(agent.Location{}, agent.Location{}, "站", agent.TravelDriving), "key") {
		t.Fatal("the navigation link must never carry a credential")
	}
}

func TestTransportFailuresNeverRevealTheServerKey(t *testing.T) {
	// A closed loopback port makes http.Client fail before any byte is sent,
	// and it reports that failure as a *url.Error carrying the full URL.
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	address := listener.Addr().String()
	_ = listener.Close()

	const key = "secret-server-key-0123456789"
	var logs bytes.Buffer
	client := NewClient(ClientConfig{
		ServerKey: key,
		BaseURL:   "http://" + address,
		Logger:    slog.New(slog.NewTextHandler(&logs, nil)),
	})

	_, err = client.SearchPois(context.Background(), agent.PoiQuery{
		Location: agent.Location{LatitudeE6: 39977680, LongitudeE6: 116316417},
		Keyword:  "充电",
		Limit:    1,
	})
	if !errors.Is(err, errProviderUnavailable) {
		t.Fatalf("SearchPois() error = %v, want the provider-unavailable error", err)
	}
	if strings.Contains(err.Error(), key) {
		t.Fatalf("the returned error carries the key: %v", err)
	}
	if strings.Contains(logs.String(), key) {
		t.Fatalf("the log line carries the key: %s", logs.String())
	}
	if !strings.Contains(logs.String(), "could not be reached") {
		t.Fatalf("the failure was not logged: %s", logs.String())
	}
}
