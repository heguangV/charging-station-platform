package geo

import (
	"context"
	"encoding/json"
	"fmt"
	"net/url"

	"github.com/heguangV/charging-station-platform/backend/internal/agent"
)

// The datum conversion between the browser and the platform.
//
// Browser geolocation returns WGS-84; every coordinate the platform stores,
// returns and sends to the map provider is GCJ-02. In this part of the world
// the two differ by up to several hundred metres, which is the distance between
// the station across the road and the one two streets away.
//
// The conversion is delegated to the provider rather than computed locally.
// An offline algorithm exists but is an approximation that must not be applied
// near the border of the offset region, and getting it wrong is silent: the
// answer simply describes a slightly different place. A provider call that
// fails is recoverable; a conversion that is quietly wrong is not.

// translatePath is the provider's coordinate translation endpoint.
const translatePath = "/ws/coord/v1/translate"

// translateTypeGPS is the provider's code for a WGS-84 to GCJ-02 conversion.
const translateTypeGPS = "1"

// translatedPoint is one entry of the translation response.
type translatedPoint struct {
	Latitude  *float64 `json:"lat"`
	Longitude *float64 `json:"lng"`
}

// ToGCJ02 converts a WGS-84 position into the platform's datum.
//
// The result is validated rather than trusted: a provider that answers with
// something outside the coordinate range, or with a body that does not carry
// exactly one point, has not performed the conversion, and returning the input
// unchanged would be worse than reporting the failure - the caller would go on
// to search from a position it believes was corrected.
func (c *Client) ToGCJ02(position agent.Location) (agent.Location, error) {
	parameters := url.Values{}
	parameters.Set("locations", coordinate(position.LatitudeE6, position.LongitudeE6))
	parameters.Set("type", translateTypeGPS)

	frame, err := c.get(context.Background(), translatePath, parameters)
	if err != nil {
		return agent.Location{}, err
	}

	var points []translatedPoint
	if len(frame.Locations) == 0 {
		return agent.Location{}, fmt.Errorf("%w: the translation response carried no locations", errProviderUnavailable)
	}
	if err := json.Unmarshal(frame.Locations, &points); err != nil {
		return agent.Location{}, fmt.Errorf("%w: the translation response was not readable", errProviderUnavailable)
	}
	if len(points) != 1 || points[0].Latitude == nil || points[0].Longitude == nil {
		return agent.Location{}, fmt.Errorf("%w: the translation response did not carry exactly one point", errProviderUnavailable)
	}
	latitude := *points[0].Latitude
	longitude := *points[0].Longitude
	if latitude < -90 || latitude > 90 || longitude < -180 || longitude > 180 {
		return agent.Location{}, fmt.Errorf("%w: the translated point is outside the coordinate range", errProviderUnavailable)
	}
	return agent.Location{LatitudeE6: e6(latitude), LongitudeE6: e6(longitude)}, nil
}
