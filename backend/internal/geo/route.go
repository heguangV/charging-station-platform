package geo

import (
	"context"
	"encoding/json"
	"fmt"
	"net/url"
	"strings"

	"github.com/heguangV/charging-station-platform/backend/internal/agent"
)

// Route planning, on the provider's direction endpoint.
//
// The rule that shapes this file is that a route is never a reason to fail a
// request. When the provider is unconfigured, unreachable, slow or answers with
// something unusable, the caller receives the straight-line estimate with
// Fallback set, and the assistant reports a degraded answer. A driver told "the
// map service is down, here is the straight-line distance" can act on that; a
// driver shown an error cannot.

const (
	// directionPathPrefix precedes the travel mode in the provider's URL.
	directionPathPrefix = "/ws/direction/v1/"

	// maximumPolylinePoints bounds the decoded path. A path is drawn, not
	// analysed, and a provider that returns tens of thousands of points is
	// answering a different question.
	maximumPolylinePoints = 2000

	// maximumSteps bounds the instructions. The same reasoning as the polyline.
	maximumSteps = 50

	// degenerateRouteMeter is the distance below which two positions are the
	// same place. Below it the provider is not called at all: a route from a
	// point to itself is not a route, and asking would spend a request to be
	// told so.
	degenerateRouteMeter = 5
)

// directionModeSegment maps a travel mode to the provider's URL segment.
func directionModeSegment(mode agent.TravelMode) string {
	switch mode {
	case agent.TravelWalking:
		return "walking"
	case agent.TravelTransit:
		return "transit"
	default:
		return "driving"
	}
}

// browserRouteType maps a travel mode to the navigation link's type parameter.
func browserRouteType(mode agent.TravelMode) string {
	switch mode {
	case agent.TravelWalking:
		return "walk"
	case agent.TravelTransit:
		return "bus"
	default:
		return "drive"
	}
}

// plannedRoute is the provider's answer, decoded.
type plannedRoute struct {
	distanceMeter  int64
	durationSecond int64
	polyline       []agent.RoutePoint
	steps          []agent.RouteStep
}

// PlanRoute returns a route to the request's destination.
//
// The result always carries a usable distance, a navigation link and an honest
// Fallback flag. An error is returned only for input the platform itself
// refuses, never for a provider that could not answer.
func (c *Client) PlanRoute(ctx context.Context, request agent.RouteRequest) (agent.Route, error) {
	origin := request.Origin
	destination := request.Destination
	if origin.LatitudeE6 < -90_000_000 || origin.LatitudeE6 > 90_000_000 ||
		origin.LongitudeE6 < -180_000_000 || origin.LongitudeE6 > 180_000_000 {
		return agent.Route{}, fmt.Errorf("geo: the origin is outside the coordinate range")
	}
	if destination.LatitudeE6 < -90_000_000 || destination.LatitudeE6 > 90_000_000 ||
		destination.LongitudeE6 < -180_000_000 || destination.LongitudeE6 > 180_000_000 {
		return agent.Route{}, fmt.Errorf("geo: the destination is outside the coordinate range")
	}

	name := strings.TrimSpace(request.DestinationName)
	if name == "" {
		name = "目的地"
	}
	route := agent.Route{
		DestinationName: name,
		Origin:          origin,
		Destination:     destination,
		Provider:        agent.ProviderLocalFallback,
		Fallback:        true,
	}

	directDistance := HaversineMeter(origin.LatitudeE6, origin.LongitudeE6,
		destination.LatitudeE6, destination.LongitudeE6)
	if directDistance > degenerateRouteMeter {
		if planned, ok := c.plan(ctx, origin, destination, request.Mode); ok && usablePlannedRoute(planned) {
			route.DistanceMeter = planned.distanceMeter
			route.DurationSecond = planned.durationSecond
			route.Polyline = planned.polyline
			route.Steps = planned.steps
			route.Provider = agent.ProviderTencentMap
			route.Fallback = false
		}
	}
	if route.Fallback || len(route.Polyline) == 0 || route.DistanceMeter <= 1 {
		// The estimate, stated as one: the straight line between the two points
		// with no duration, because a straight line has no speed.
		route.Fallback = true
		route.Provider = agent.ProviderLocalFallback
		route.DistanceMeter = directDistance
		route.DurationSecond = 0
		route.Polyline = []agent.RoutePoint{
			{LatitudeE6: origin.LatitudeE6, LongitudeE6: origin.LongitudeE6},
			{LatitudeE6: destination.LatitudeE6, LongitudeE6: destination.LongitudeE6},
		}
		route.Steps = nil
	}

	route.BrowserURL = BrowserRouteURL(route.Origin, route.Destination, name, request.Mode)
	return route, nil
}

// plan performs the provider call and decodes it, reporting whether it produced
// anything usable.
func (c *Client) plan(ctx context.Context, origin, destination agent.Location, mode agent.TravelMode) (plannedRoute, bool) {
	if !c.Configured() {
		return plannedRoute{}, false
	}

	parameters := url.Values{}
	parameters.Set("from", coordinate(origin.LatitudeE6, origin.LongitudeE6))
	parameters.Set("to", coordinate(destination.LatitudeE6, destination.LongitudeE6))

	frame, err := c.get(ctx, directionPathPrefix+directionModeSegment(mode), parameters)
	if err != nil {
		return plannedRoute{}, false
	}
	return parsePlannedRoute(frame.Result, mode)
}

// directionResponse is the provider's direction payload.
type directionResponse struct {
	Routes []struct {
		Distance *float64        `json:"distance"`
		Duration *float64        `json:"duration"`
		Polyline json.RawMessage `json:"polyline"`
		Steps    json.RawMessage `json:"steps"`
	} `json:"routes"`
}

// parsePlannedRoute decodes a direction payload.
func parsePlannedRoute(raw json.RawMessage, mode agent.TravelMode) (plannedRoute, bool) {
	if len(raw) == 0 {
		return plannedRoute{}, false
	}
	var payload directionResponse
	if err := json.Unmarshal(raw, &payload); err != nil || len(payload.Routes) == 0 {
		return plannedRoute{}, false
	}
	first := payload.Routes[0]
	if first.Distance == nil || first.Duration == nil {
		return plannedRoute{}, false
	}

	route := plannedRoute{
		distanceMeter: int64(*first.Distance),
		// The provider states duration in minutes. Everything above this package
		// speaks seconds, and a duration that is silently sixty times too small
		// would make every route look instantaneous.
		durationSecond: int64(*first.Duration * 60),
	}
	if route.distanceMeter <= 1 || route.durationSecond <= 0 {
		return plannedRoute{}, false
	}

	if mode == agent.TravelTransit {
		// A transit route nests its legs, each with its own path and steps.
		var segments []struct {
			Polyline json.RawMessage `json:"polyline"`
			Steps    json.RawMessage `json:"steps"`
		}
		if err := json.Unmarshal(first.Steps, &segments); err != nil {
			return plannedRoute{}, false
		}
		for _, segment := range segments {
			route.polyline = appendPolyline(route.polyline, segment.Polyline)
			route.steps = appendSteps(route.steps, segment.Steps)
		}
	} else {
		route.polyline = appendPolyline(route.polyline, first.Polyline)
		route.steps = appendSteps(route.steps, first.Steps)
	}
	return route, true
}

// usablePlannedRoute reports whether a provider answer is a route rather than a
// status code that happened to be zero.
//
// It is the check that keeps a degenerate answer - a distance of a metre, no
// duration, a path of one repeated point - from being presented as a real
// route. The platform's own estimate is better than any of those.
func usablePlannedRoute(route plannedRoute) bool {
	if route.distanceMeter <= 1 || route.durationSecond <= 0 || len(route.polyline) < 2 {
		return false
	}
	first := route.polyline[0]
	moved := false
	for _, point := range route.polyline {
		if point.LatitudeE6 < -90_000_000 || point.LatitudeE6 > 90_000_000 ||
			point.LongitudeE6 < -180_000_000 || point.LongitudeE6 > 180_000_000 {
			return false
		}
		if point.LatitudeE6 != first.LatitudeE6 || point.LongitudeE6 != first.LongitudeE6 {
			moved = true
		}
	}
	return moved
}

// appendPolyline decodes one encoded path and appends it.
//
// The encoding is relative: the first pair is absolute and every later pair is
// an offset from the pair two places before it. A segment that starts where the
// previous one ended would otherwise draw a duplicate point, so the shared
// point is dropped.
func appendPolyline(target []agent.RoutePoint, encoded json.RawMessage) []agent.RoutePoint {
	points := decodePolyline(encoded)
	if len(target) > 0 && len(points) > 0 &&
		target[len(target)-1] == points[0] {
		points = points[1:]
	}
	remaining := maximumPolylinePoints - len(target)
	if remaining < 0 {
		remaining = 0
	}
	if len(points) > remaining {
		points = points[:remaining]
	}
	return append(target, points...)
}

// decodePolyline turns the provider's relative encoding into absolute points.
func decodePolyline(encoded json.RawMessage) []agent.RoutePoint {
	if len(encoded) == 0 {
		return nil
	}
	var values []float64
	if err := json.Unmarshal(encoded, &values); err != nil {
		return nil
	}
	if len(values) < 4 || len(values)%2 != 0 {
		return nil
	}
	for index := 2; index < len(values); index++ {
		values[index] = values[index-2] + values[index]/1e6
	}

	count := len(values) / 2
	if count > maximumPolylinePoints {
		count = maximumPolylinePoints
	}
	points := make([]agent.RoutePoint, 0, count)
	for index := 0; index < count; index++ {
		latitude := values[index*2]
		longitude := values[index*2+1]
		// A point outside the world is a decoding error, not a place: keeping the
		// rest of the path would draw a line through nowhere.
		if latitude < -90 || latitude > 90 || longitude < -180 || longitude > 180 {
			return nil
		}
		points = append(points, agent.RoutePoint{
			LatitudeE6:  e6(latitude),
			LongitudeE6: e6(longitude),
		})
	}
	return points
}

// appendSteps decodes one instruction list and appends it.
//
// A step that carries its own nested path is a transit leg rather than an
// instruction; its contents are handled by the caller, so it is skipped here
// instead of being rendered as an empty sentence.
func appendSteps(target []agent.RouteStep, raw json.RawMessage) []agent.RouteStep {
	if len(raw) == 0 {
		return target
	}
	var entries []map[string]any
	if err := json.Unmarshal(raw, &entries); err != nil {
		return target
	}
	for _, entry := range entries {
		if len(target) >= maximumSteps {
			return target
		}
		if _, nested := entry["polyline"]; nested {
			continue
		}
		instruction, _ := entry["instruction"].(string)
		instruction = strings.TrimSpace(instruction)
		if instruction == "" {
			continue
		}
		if len([]rune(instruction)) > 300 {
			instruction = string([]rune(instruction)[:300])
		}
		distance, _ := entry["distance"].(float64)
		duration, _ := entry["duration"].(float64)
		target = append(target, agent.RouteStep{
			Instruction:   instruction,
			DistanceMeter: int64(distance),
			// Minutes again.
			DurationSecond: int64(duration * 60),
		})
	}
	return target
}

// BrowserRouteURL builds the navigation link the user opens.
//
// The link is a hand-off to the provider's own web page, so it carries the two
// endpoints and nothing else - no key, no session token, no identifier. The
// origin is labelled rather than named because the platform does not know where
// the user is standing.
func BrowserRouteURL(origin, destination agent.Location, destinationName string, mode agent.TravelMode) string {
	return "https://apis.map.qq.com/uri/v1/routeplan?type=" + browserRouteType(mode) +
		"&from=" + percentEncode("导航起点") +
		"&fromcoord=" + percentEncode(coordinate(origin.LatitudeE6, origin.LongitudeE6)) +
		"&to=" + percentEncode(destinationName) +
		"&tocoord=" + percentEncode(coordinate(destination.LatitudeE6, destination.LongitudeE6)) +
		"&referer=NCS"
}

// percentEncode escapes a value for the navigation link.
//
// It follows RFC 3986 for the unreserved set and additionally leaves the comma
// alone, because a coordinate pair is more readable - and in some clients more
// reliable - unescaped. The destination name is usually Chinese, so the
// encoding works on bytes and every byte outside the literal set is escaped.
func percentEncode(value string) string {
	const hexDigits = "0123456789ABCDEF"
	var encoded strings.Builder
	encoded.Grow(len(value))
	for index := 0; index < len(value); index++ {
		character := value[index]
		literal := (character >= 'a' && character <= 'z') ||
			(character >= 'A' && character <= 'Z') ||
			(character >= '0' && character <= '9') ||
			character == '-' || character == '_' || character == '.' ||
			character == '~' || character == ','
		if literal {
			encoded.WriteByte(character)
			continue
		}
		encoded.WriteByte('%')
		encoded.WriteByte(hexDigits[character>>4])
		encoded.WriteByte(hexDigits[character&0x0F])
	}
	return encoded.String()
}
