#include "core/application/navigation_service.h"

#include <algorithm>

namespace ncs::core::application
{
namespace
{
bool usableRoute(const PlannedRoute& route)
{
    if (route.distanceMeter <= 1 || route.durationSecond <= 0 || route.polyline.size() < 2)
        return false;
    const auto first = route.polyline.front();
    return std::all_of(route.polyline.begin(), route.polyline.end(),
                       [](RoutePoint p)
                       {
                           return p.latitudeE6 >= -90000000 && p.latitudeE6 <= 90000000 &&
                                  p.longitudeE6 >= -180000000 && p.longitudeE6 <= 180000000;
                       }) &&
           std::any_of(route.polyline.begin(), route.polyline.end(),
                       [first](RoutePoint p) {
                           return p.latitudeE6 != first.latitudeE6 ||
                                  p.longitudeE6 != first.longitudeE6;
                       });
}
} // namespace

std::string_view travelModeName(const TravelMode mode)
{
    switch (mode)
    {
    case TravelMode::Driving:
        return "driving";
    case TravelMode::Walking:
        return "walking";
    case TravelMode::Transit:
        return "transit";
    }
    return "driving";
}

ServiceResult<NavigationResult> NavigationService::routeToStation(
    const std::int64_t stationId, const std::optional<std::int64_t> latitudeE6,
    const std::optional<std::int64_t> longitudeE6, const std::string& keyword,
    const TravelMode mode, const bool gpsOrigin)
{
    const auto station = repository_.station(stationId);
    if (!station)
    {
        return {core::domain::ErrorCode::NotFound, std::nullopt};
    }

    NavigationResult result;
    result.stationId = station->id;
    result.stationName = station->name;
    result.destinationAddress = station->address;
    result.destination = {station->latitudeE6, station->longitudeE6};
    result.mode = mode;
    result.locationFallback = !latitudeE6 || !longitudeE6;
    if (latitudeE6 && longitudeE6)
    {
        result.origin = {*latitudeE6, *longitudeE6};
    }
    else if (!keyword.empty())
    {
        if (const auto resolved = geocoder_.resolve(keyword))
        {
            result.origin = {resolved->latitudeE6, resolved->longitudeE6};
            result.locationFallback = false;
        }
    }
    if (gpsOrigin)
    {
        if (!latitudeE6 || !longitudeE6)
            return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
        const auto normalized = routePlanner_.normalizeGps(result.origin);
        if (!normalized)
            return {core::domain::ErrorCode::ExternalServiceUnavailable, std::nullopt};
        result.origin = *normalized;
    }
    if (result.locationFallback)
    {
        result.origin = {StationService::defaultLatitudeE6, StationService::defaultLongitudeE6};
    }

    const auto directDistance = StationService::haversineMeter(
        result.origin.latitudeE6, result.origin.longitudeE6, result.destination.latitudeE6,
        result.destination.longitudeE6);
    const auto route = directDistance > 5
                           ? routePlanner_.plan(result.origin, result.destination, mode)
                           : std::nullopt;
    if (route && usableRoute(*route))
    {
        result.distanceMeter = route->distanceMeter;
        result.durationSecond = route->durationSecond;
        result.polyline = route->polyline;
        result.steps = route->steps;
        return {core::domain::ErrorCode::Ok, std::move(result)};
    }

    result.routeFallback = true;
    result.distanceMeter = directDistance;
    result.polyline = {result.origin, result.destination};
    return {core::domain::ErrorCode::Ok, std::move(result)};
}

} // namespace ncs::core::application
