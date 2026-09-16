package agent

import (
	"context"
	"encoding/json"
	"math"
	"strconv"
	"strings"
)

// The tool abstraction: every tool declares a name, a description for the
// model, an argument schema, and one invocation.
//
// The service dispatches only through this interface, so adding a capability
// never touches the orchestration and the transport never learns how a tool
// works. A tool reads through the domain ports above; it never opens a database
// and never reimplements an order, a charge or a balance rule.
//
// Invoke returns a result rather than an error on purpose. A tool that fails is
// an expected outcome of a conversation, not an exception: the service degrades
// that one answer and carries on with the other tools, and the caller must
// never be handed an internal failure reason. Making the failure a value is
// what keeps a caller from returning early and swallowing the rest.

// Tool is one capability the model may ask for.
type Tool interface {
	Name() string
	Description() string
	// ParametersSchema is the JSON object describing the arguments, as the model
	// providers expect it.
	ParametersSchema() string
	Invoke(ctx context.Context, conv Context, args Arguments) ToolResult
}

// Arguments is the argument object a model produced. It is untrusted input:
// nothing here is used before a range check.
type Arguments map[string]any

// ToolResult is one invocation's outcome.
type ToolResult struct {
	// OK reports whether the tool could look. A successful search that found
	// nothing is OK with empty data; a provider that could not be reached is
	// not OK.
	OK bool
	// Err is the internal reason. It goes to the log and to the next tool's
	// coordination, never to the client.
	Err string
	// Observation is the compact text handed back to the model as evidence.
	Observation string
	// Stations, Pois and Route are the structured findings.
	Stations []StationSummary
	Pois     []Poi
	Route    *Route
}

// toolFailure builds a failed result. The observation is written for the model,
// so it says what to try next rather than what broke internally.
func toolFailure(reason, observation string) ToolResult {
	return ToolResult{OK: false, Err: reason, Observation: observation}
}

// —— argument readers ——
//
// Every value a model supplies is treated as untrusted: out of range, wrongly
// typed or missing all yield "not usable", and the tool then decides whether a
// default applies. Returning a zero value instead of an error keeps the call
// sites readable, and returning a separate "was it usable" flag is what stops a
// legitimate zero from being confused with a missing argument.

// hasArgument reports whether the key is present at all, which is how a tool
// tells "the model said 0" from "the model said nothing".
func hasArgument(args Arguments, key string) bool {
	_, present := args[key]
	return present
}

// intArgument reads an integer inside a range. JSON numbers arrive as
// json.Number (the endpoint decodes with UseNumber) or float64, and a model
// sometimes quotes the number, so both forms are accepted.
func intArgument(args Arguments, key string, minimum, maximum int64) (int64, bool) {
	value, present := args[key]
	if !present || value == nil {
		return 0, false
	}
	var number int64
	switch typed := value.(type) {
	case json.Number:
		parsed, err := typed.Int64()
		if err != nil {
			// A number written as 1.0 is still an integer; 1.5 is not, and
			// must not be silently truncated into a station id or a coordinate.
			asFloat, floatErr := typed.Float64()
			if floatErr != nil || !isWholeNumber(asFloat) {
				return 0, false
			}
			number = int64(asFloat)
		} else {
			number = parsed
		}
	case float64:
		if !isWholeNumber(typed) {
			return 0, false
		}
		number = int64(typed)
	case int64:
		number = typed
	case int:
		number = int64(typed)
	case string:
		parsed, err := strconv.ParseInt(strings.TrimSpace(typed), 10, 64)
		if err != nil {
			return 0, false
		}
		number = parsed
	default:
		return 0, false
	}
	if number < minimum || number > maximum {
		return 0, false
	}
	return number, true
}

// isWholeNumber reports whether value is finite, has no fractional part and
// converts to int64 without overflow, so int64(value) is exact.
func isWholeNumber(value float64) bool {
	if math.IsNaN(value) || math.IsInf(value, 0) || value != math.Trunc(value) {
		return false
	}
	return value >= math.MinInt64 && value < math.MaxInt64
}

// stringArgument reads a trimmed string of at most maximumLength bytes. Empty
// and over-long values are reported as unusable rather than truncated: a
// keyword cut in half would search for something the user never asked for.
func stringArgument(args Arguments, key string, maximumLength int) string {
	value, present := args[key]
	if !present || value == nil {
		return ""
	}
	text, ok := value.(string)
	if !ok {
		return ""
	}
	text = strings.TrimSpace(text)
	if text == "" || len(text) > maximumLength {
		return ""
	}
	return text
}

// travelModeArgument reads a travel mode, reporting whether it was usable so a
// caller can tell "driving" from "unspecified".
func travelModeArgument(args Arguments, key string) (TravelMode, bool) {
	text := stringArgument(args, key, 16)
	switch TravelMode(text) {
	case TravelDriving, TravelWalking, TravelTransit:
		return TravelMode(text), true
	default:
		return "", false
	}
}

// chargerTypeRestriction maps the client's 0/1 preference to the connector
// type the station queries use. The client speaks 0 for slow and 1 for fast;
// the platform speaks AC and DC.
func chargerTypeRestriction(preference int) string {
	if preference == 1 {
		return "DC"
	}
	return "AC"
}
