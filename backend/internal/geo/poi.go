package geo

import (
	"context"
	"encoding/json"
	"fmt"
	"net/url"
	"strings"

	"github.com/heguangV/charging-station-platform/backend/internal/agent"
)

// Nearby place search, on the provider's place endpoint.
//
// The assistant asks for restaurants, coffee shops, convenience stores and
// shopping centres around a point - typically the charging station a driver is
// already heading for, because "somewhere to eat while it charges" is the
// question this answers.

// placeSearchPath is the provider's place search endpoint.
const placeSearchPath = "/ws/place/v1/search"

// The provider's own bounds, which are tighter than the platform's in one case
// and looser in the other. Both are applied here as well as upstream: this is
// the last point before the provider sees the values.
const (
	maxProviderRadiusMeter = 10_000
	minProviderRadiusMeter = 100
	maxProviderLimit       = 20
)

// defaultSearchKeyword is what the provider searches for when neither a
// keyword nor a recognised category is given. A place search always needs a
// term, and asking for nothing would return the nearest bus stop.
const defaultSearchKeyword = "餐厅"

// providerCategoryKeywords maps the platform's categories to the provider's
// vocabulary. The user's category is a business concept; the provider's is a
// search term, and the two do not share a spelling.
var providerCategoryKeywords = map[string]string{
	"餐饮":  "餐厅",
	"餐厅":  "餐厅",
	"吃饭":  "餐厅",
	"美食":  "餐厅",
	"咖啡":  "咖啡厅",
	"咖啡厅": "咖啡厅",
	"咖啡店": "咖啡厅",
	"便利店": "便利店",
	"商场":  "购物中心",
	"购物":  "购物中心",
}

// poiEntry is one place as the provider returns it.
type poiEntry struct {
	ID       string `json:"id"`
	Title    string `json:"title"`
	Category string `json:"category"`
	Address  string `json:"address"`
	Tel      string `json:"tel"`
	TelAlt   string `json:"tel_1"`
	Location struct {
		Latitude  *float64 `json:"lat"`
		Longitude *float64 `json:"lng"`
	} `json:"location"`
	// Distance is the provider's own distance from the search centre, in
	// metres. It is not always present.
	Distance *float64 `json:"_distance"`
}

// Available reports whether a map key is configured.
func (c *Client) Available() bool { return c.Configured() }

// SearchPois returns the places around the query's centre.
func (c *Client) SearchPois(_ context.Context, query agent.PoiQuery) ([]agent.Poi, error) {
	if !c.Configured() {
		return nil, fmt.Errorf("%w: no map key is configured", errProviderUnavailable)
	}
	// A search centred on the origin is not a search: it is what an unset
	// coordinate looks like, and answering it would return whatever happens to
	// be near 0°N 0°E.
	if query.Location.LatitudeE6 == 0 && query.Location.LongitudeE6 == 0 {
		return nil, fmt.Errorf("%w: the search centre is not a position", errProviderUnavailable)
	}
	if query.Location.LatitudeE6 < -90_000_000 || query.Location.LatitudeE6 > 90_000_000 ||
		query.Location.LongitudeE6 < -180_000_000 || query.Location.LongitudeE6 > 180_000_000 {
		return nil, fmt.Errorf("%w: the search centre is outside the coordinate range", errProviderUnavailable)
	}

	keyword := strings.TrimSpace(query.Keyword)
	if keyword == "" {
		keyword = providerCategoryKeywords[strings.TrimSpace(query.Category)]
	}
	if keyword == "" {
		keyword = defaultSearchKeyword
	}
	radius := query.RadiusMeter
	if radius < minProviderRadiusMeter {
		radius = minProviderRadiusMeter
	}
	if radius > maxProviderRadiusMeter {
		radius = maxProviderRadiusMeter
	}
	limit := query.Limit
	if limit < 1 {
		limit = 1
	}
	if limit > maxProviderLimit {
		limit = maxProviderLimit
	}

	parameters := url.Values{}
	parameters.Set("keyword", keyword)
	parameters.Set("boundary", fmt.Sprintf("nearby(%s,%d)",
		coordinate(query.Location.LatitudeE6, query.Location.LongitudeE6), radius))
	parameters.Set("page_size", fmt.Sprintf("%d", limit))
	parameters.Set("page_index", "1")
	parameters.Set("orderby", "_distance")

	frame, err := c.get(context.Background(), placeSearchPath, parameters)
	if err != nil {
		return nil, err
	}
	if len(frame.Data) == 0 {
		// A successful search that matched nothing. It is not an error, and the
		// caller reports it as an empty list rather than as a failure.
		return []agent.Poi{}, nil
	}
	var entries []poiEntry
	if err := json.Unmarshal(frame.Data, &entries); err != nil {
		return nil, fmt.Errorf("%w: the place response was not readable", errProviderUnavailable)
	}

	pois := make([]agent.Poi, 0, len(entries))
	for _, entry := range entries {
		if len(pois) >= limit {
			break
		}
		// A place without coordinates cannot be shown on a map or routed to, and
		// a place without a name cannot be read. Both are dropped rather than
		// presented as a card that leads nowhere.
		if entry.Location.Latitude == nil || entry.Location.Longitude == nil {
			continue
		}
		name := strings.TrimSpace(entry.Title)
		if name == "" {
			continue
		}
		latitude := e6(*entry.Location.Latitude)
		longitude := e6(*entry.Location.Longitude)
		if latitude == 0 && longitude == 0 {
			continue
		}

		distance := int64(0)
		if entry.Distance != nil && *entry.Distance > 0 {
			distance = int64(*entry.Distance)
		}
		if distance <= 0 {
			// The provider does not always report a distance. Computing it is
			// better than showing zero, which reads as "right here".
			distance = HaversineMeter(query.Location.LatitudeE6, query.Location.LongitudeE6, latitude, longitude)
		}
		tel := strings.TrimSpace(entry.Tel)
		if tel == "" {
			tel = strings.TrimSpace(entry.TelAlt)
		}
		pois = append(pois, agent.Poi{
			ID:            strings.TrimSpace(entry.ID),
			Name:          name,
			Category:      strings.TrimSpace(entry.Category),
			Address:       strings.TrimSpace(entry.Address),
			LatitudeE6:    latitude,
			LongitudeE6:   longitude,
			DistanceMeter: distance,
			Tel:           tel,
		})
	}
	return pois, nil
}
