package agent

import (
	"time"

	"github.com/heguangV/charging-station-platform/backend/internal/station"
)

// The assistant's own value types.
//
// They are deliberately not the HTTP models: the tools produce these, the
// service assembles them, and the transport layer decides how they are
// serialized. That is what lets the tools be exercised with no server and no
// network at all.

// Limits bound one conversation.
//
// The bounds exist because a model can ask for more work than a request should
// ever do: without them a single question could issue an unbounded number of
// map calls while the caller waits. Exceeding them is not an error - the extra
// work is simply not done, and the answer says what it is based on.
type Limits struct {
	// MaxToolCalls is how many tools one conversation may run. The excess is
	// ignored rather than reported as a failure.
	MaxToolCalls int
	// MaxStations caps the stations in a response.
	MaxStations int
	// MaxPois caps the places in a response.
	MaxPois int
	// DefaultPoiRadiusMeter is the search radius when the caller names none.
	DefaultPoiRadiusMeter int
	// MaxMessageLength bounds the question the endpoint accepts.
	MaxMessageLength int
	// MaxReplyLength bounds the reply, in characters.
	MaxReplyLength int
}

// DefaultLimits are the bounds a service uses when none are configured.
func DefaultLimits() Limits {
	return Limits{
		MaxToolCalls:          4,
		MaxStations:           3,
		MaxPois:               5,
		DefaultPoiRadiusMeter: 2000,
		MaxMessageLength:      500,
		MaxReplyLength:        400,
	}
}

// Location is an E6 coordinate pair (degrees multiplied by one million). It is
// always a pair: half a position is not a position, and the tools refuse one
// rather than silently searching from the equator.
type Location struct {
	LatitudeE6  int64
	LongitudeE6 int64
}

// Context carries the facts of one conversation that did not come from the
// question itself.
//
// UserID comes from the session token and never from the request body: the
// assistant must not be able to ask about somebody else's account. RequestID is
// for log correlation and never reaches the reply.
type Context struct {
	UserID    int64
	RequestID string
	// Location is the user's own position, or nil when the client has none.
	Location *Location
	// WGS84 declares the datum of Location. Browser geolocation returns WGS-84,
	// which the map provider does not accept as-is; a client that mislabels it
	// would otherwise be answered with distances from the wrong place.
	WGS84 bool
	// ChargerType prefers 0 for AC (slow) or 1 for DC (fast); nil means no
	// preference.
	ChargerType *int
	// Now is the instant the conversation is answered at, so a relative result
	// ("open now") is decided once and can be asserted.
	Now time.Time
}

// HasLocation reports whether a usable position was supplied.
func (c Context) HasLocation() bool { return c.Location != nil }

// StationSummary is the station domain's own recommendation view.
//
// The assistant reports it unchanged rather than re-declaring the same fifteen
// fields: a station field that meant one thing in a search and another in a
// recommendation would be a bug waiting to happen, and the domain already owns
// the meaning.
type StationSummary = station.Summary

// Poi is a nearby place a map provider returned.
type Poi struct {
	ID            string
	Name          string
	Category      string
	Address       string
	LatitudeE6    int64
	LongitudeE6   int64
	DistanceMeter int64
	Tel           string
}

// TravelMode is the way a route is travelled. The values are the wire values
// the map provider expects.
type TravelMode string

// Travel modes.
const (
	TravelDriving TravelMode = "driving"
	TravelWalking TravelMode = "walking"
	TravelTransit TravelMode = "transit"
)

// TravelModeName is the Chinese label used in generated text.
func TravelModeName(mode TravelMode) string {
	switch mode {
	case TravelWalking:
		return "步行"
	case TravelTransit:
		return "公交"
	default:
		return "驾车"
	}
}

// RouteStep is one instruction of a route.
type RouteStep struct {
	Instruction    string
	DistanceMeter  int64
	DurationSecond int64
}

// RoutePoint is one point of a route's polyline.
type RoutePoint struct {
	LatitudeE6  int64
	LongitudeE6 int64
}

// Route is a planned journey to a destination.
//
// Fallback marks the answer given when the map provider is unavailable: the
// straight-line estimate. It is reported as degraded rather than passed off as
// a route, because a caller who believes a straight line is a road will plan
// the wrong trip.
type Route struct {
	DestinationName string
	Origin          Location
	Destination     Location
	DistanceMeter   int64
	DurationSecond  int64
	Provider        string
	Fallback        bool
	Steps           []RouteStep
	Polyline        []RoutePoint
	BrowserURL      string
}

// Route providers.
const (
	ProviderTencentMap    = "TENCENT_MAP"
	ProviderLocalFallback = "LOCAL_FALLBACK"
)

// Action is a button the client may offer next.
type Action struct {
	// Type is open_station to navigate inside the app, or navigate to open URL.
	Type string
	// Label is the button text.
	Label string
	// TargetID is the station id, for open_station.
	TargetID string
	// URL is the navigation link, for navigate. It carries no credential.
	URL string
}

// Action types.
const (
	ActionOpenStation = "open_station"
	ActionNavigate    = "navigate"
)

// Result is one answer: the reply, the data behind it, and whether a degraded
// path produced it.
//
// An empty collection means the platform looked and found nothing. A caller
// must never receive an empty object standing in for a failure.
type Result struct {
	Reply    string
	Stations []StationSummary
	Pois     []Poi
	Route    *Route
	Actions  []Action
	// Tools lists the tools actually executed, in execution order.
	Tools []string
	// LLMUsed reports whether the reply came from the model.
	LLMUsed bool
	// Degraded reports whether the model or the map service was unavailable.
	Degraded bool
}
