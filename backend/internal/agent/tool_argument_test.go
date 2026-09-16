package agent

import (
	"encoding/json"
	"math"
	"testing"
)

// A model's argument object is untrusted, and JSON numbers reach the tools as
// json.Number (the endpoint decodes with UseNumber) or float64. Whole numbers
// in any spelling are usable; anything with a fraction, or not finite, is not,
// because int64() would silently truncate it into a different id or position.
func TestIntArgumentAcceptsWholeNumbersInEverySpelling(t *testing.T) {
	cases := map[string]any{
		"json integer":  json.Number("12"),
		"json 1.0":      json.Number("12.0"),
		"json exponent": json.Number("1.2e1"),
		"float64":       float64(12),
		"int64":         int64(12),
		"int":           12,
		"quoted":        " 12 ",
	}
	for name, value := range cases {
		got, ok := intArgument(Arguments{"stationId": value}, "stationId", 1, 100)
		if !ok || got != 12 {
			t.Errorf("%s: intArgument() = %d, %v; want 12, true", name, got, ok)
		}
	}
}

func TestIntArgumentRejectsFractionsAndNonFiniteNumbers(t *testing.T) {
	cases := map[string]any{
		"json fraction":     json.Number("12.7"),
		"json tiny":         json.Number("0.5"),
		"json huge":         json.Number("1e30"),
		"float fraction":    12.7,
		"float NaN":         math.NaN(),
		"float +Inf":        math.Inf(1),
		"float -Inf":        math.Inf(-1),
		"float overflow":    1e30,
		"quoted fraction":   "12.7",
		"bool":              true,
		"nil":               nil,
		"below the minimum": json.Number("0"),
		"above the maximum": json.Number("101"),
	}
	for name, value := range cases {
		if got, ok := intArgument(Arguments{"stationId": value}, "stationId", 1, 100); ok {
			t.Errorf("%s: intArgument() = %d, true; want not usable", name, got)
		}
	}
}
