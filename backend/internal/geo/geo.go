// Package geo adapts the Tencent Map WebService to the assistant's map ports.
//
// It is the only place in the platform that holds the map Server Key, and the
// only place that speaks the provider's wire format. Everything above it -
// the assistant, its tools and the endpoint - works with the platform's own
// value types and knows nothing about the provider.
//
// Two rules run through the whole package and they are not stylistic:
//
//   - The Server Key never leaves the process. It is not logged, not echoed in
//     an error, and not part of any URL the browser receives.
//   - A provider failure is never a request failure. Every call degrades to a
//     local estimate and says so, because an assistant that cannot reach a map
//     service still knows the straight-line distance between two points.
package geo

import (
	"math"
)

// earthRadiusMeter is the mean Earth radius used by the distance estimate.
const earthRadiusMeter = 6371000.0

// HaversineMeter is the great-circle distance between two E6 positions.
//
// It is the platform's own estimate, used both as the POI distance when the
// provider omits one and as the whole route when the provider is unreachable.
// It is deliberately the same formula the station list uses to order results,
// so a distance shown in an answer and the order of the list can never
// disagree.
func HaversineMeter(latitudeE6A, longitudeE6A, latitudeE6B, longitudeE6B int64) int64 {
	latitudeA := math.Pi / 180 * (float64(latitudeE6A) / 1e6)
	longitudeA := math.Pi / 180 * (float64(longitudeE6A) / 1e6)
	latitudeB := math.Pi / 180 * (float64(latitudeE6B) / 1e6)
	longitudeB := math.Pi / 180 * (float64(longitudeE6B) / 1e6)

	deltaLatitude := latitudeB - latitudeA
	deltaLongitude := longitudeB - longitudeA
	// The clamping is not decoration: floating-point error at antipodal points
	// can push the argument of asin slightly above 1, and a NaN distance would
	// propagate into the answer.
	haversine := math.Sin(deltaLatitude/2)*math.Sin(deltaLatitude/2) +
		math.Cos(latitudeA)*math.Cos(latitudeB)*
			math.Sin(deltaLongitude/2)*math.Sin(deltaLongitude/2)
	return int64(math.Round(2 * earthRadiusMeter * math.Asin(math.Min(1, math.Sqrt(haversine)))))
}

// coordinate renders an E6 position the way the provider expects it: six
// decimal places, latitude first.
func coordinate(latitudeE6, longitudeE6 int64) string {
	return decimal6(latitudeE6) + "," + decimal6(longitudeE6)
}

// decimal6 formats an E6 integer as a fixed six-decimal string.
//
// It is done by hand rather than with strconv so a negative coordinate cannot
// lose its sign or its leading zeros: "-0.500000" and "0.050000" both have to
// survive, and a coordinate that arrives as "0.5" is a different place from the
// one that was asked about once it is parsed as degrees.
func decimal6(valueE6 int64) string {
	negative := valueE6 < 0
	magnitude := valueE6
	if negative {
		magnitude = -magnitude
	}
	whole := magnitude / 1000000
	fraction := magnitude % 1000000
	digits := make([]byte, 0, 8)
	if negative {
		digits = append(digits, '-')
	}
	digits = appendInt(digits, whole)
	digits = append(digits, '.')
	for divisor := int64(100000); divisor > 0; divisor /= 10 {
		digits = append(digits, byte('0'+fraction/divisor))
		fraction %= divisor
	}
	return string(digits)
}

func appendInt(target []byte, value int64) []byte {
	if value == 0 {
		return append(target, '0')
	}
	var buffer [20]byte
	position := len(buffer)
	for value > 0 {
		position--
		buffer[position] = byte('0' + value%10)
		value /= 10
	}
	return append(target, buffer[position:]...)
}

// e6 converts a decimal degree value to the E6 integer form.
func e6(degrees float64) int64 {
	return int64(math.Round(degrees * 1e6))
}
