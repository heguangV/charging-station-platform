package agent

import (
	"context"

	"github.com/heguangV/charging-station-platform/backend/internal/station"
)

// The capabilities the assistant consumes.
//
// They are declared here, by the consumer, rather than next to their
// implementations: the assistant is the only caller, so it is the only place
// that knows how narrow they can be. That is also what keeps the tools
// testable with a fake - the map provider and the model are the two things a
// unit test must never reach for.
//
// The direction of the dependency matters as much as the shape: the assistant
// reads stations, places and routes, and never writes an order, a charger or a
// wallet. Every port below is a query.

// StationDirectory reads stations and their chargers.
//
// The signatures are the station domain's own, so the domain service satisfies
// this interface directly and no adapter has to translate one set of names into
// another - translation layers are where a field quietly stops being updated.
//
// Implementations must never invent a station, and must return an empty slice -
// not an error - when a search genuinely matches nothing.
type StationDirectory interface {
	SearchSummaries(ctx context.Context, filter station.SummaryFilter) ([]station.Summary, error)
	GetStationSummary(ctx context.Context, stationID int64, chargerType string, chargerLimit int) (station.SummaryDetail, error)
}

// CoordinateConverter normalizes a browser position to the datum the platform's
// own data uses.
//
// The browser's geolocation returns WGS-84, and every coordinate the platform
// stores, returns and sends to the map provider is GCJ-02. The two differ by
// up to several hundred metres, which is the difference between the station
// across the road and the one two streets away, so the conversion happens once,
// here, rather than being trusted to each caller.
type CoordinateConverter interface {
	// ToGCJ02 converts a WGS-84 position.
	ToGCJ02(position Location) (Location, error)
}

// PoiProvider finds places around a point.
//
// Available reports whether the provider is configured at all. A provider that
// is not configured is not a failure of the request: the assistant says so and
// answers from what it does have, which is why the two are separate calls
// rather than one that returns an error.
type PoiProvider interface {
	Available() bool
	SearchPois(ctx context.Context, query PoiQuery) ([]Poi, error)
}

// PoiQuery is a validated place search, in the datum the provider expects.
type PoiQuery struct {
	Location Location
	// Keyword is a free-text search term; it takes precedence over Category.
	Keyword string
	// Category is one of the provider's categories, or empty.
	Category string
	// RadiusMeter and Limit are already inside their contract ranges.
	RadiusMeter int64
	Limit       int
}

// RoutePlanner estimates a journey.
//
// PlanRoute answers even when no provider is configured: it returns the
// straight-line fallback, with Fallback set so the caller can say so. That is
// why there is no Available here - the caller learns the truth from the result
// rather than having to ask two questions and reconcile them.
type RoutePlanner interface {
	PlanRoute(ctx context.Context, request RouteRequest) (Route, error)
}

// RouteRequest is a validated route query, in the datum the provider expects.
type RouteRequest struct {
	Origin      Location
	Destination Location
	Mode        TravelMode
	// DestinationName labels the route in text and in the navigation link.
	DestinationName string
}

// LLMClient is an optional chat model.
//
// Available reports whether a model is configured. When it is not, the
// assistant still answers: the deterministic planner picks the tools and the
// rule-based wording writes the reply. That is the difference between a feature
// that is off and a feature that is broken, and the response says which.
type LLMClient interface {
	Available() bool
	Chat(ctx context.Context, request ChatRequest) (ChatResponse, error)
}

// Role is a chat message's author.
type Role string

// Chat roles.
const (
	RoleSystem Role = "system"
	RoleUser   Role = "user"
)

// Message is one chat turn.
type Message struct {
	Role    Role
	Content string
}

// ToolSpec describes one tool to the model, in the provider's own format.
type ToolSpec struct {
	Name           string
	Description    string
	ParametersJSON string
}

// ToolCall is one tool the model asked for. ArgumentsJSON is the raw object the
// model produced and is treated as untrusted input.
type ToolCall struct {
	Name          string
	ArgumentsJSON string
}

// ChatRequest is one completion request. Tools is empty for the second call,
// which asks for wording rather than for more work.
type ChatRequest struct {
	Messages []Message
	Tools    []ToolSpec
}

// ChatResponse is one completion.
type ChatResponse struct {
	Content   string
	ToolCalls []ToolCall
}
