#include "core/application/navigation_service.h"

namespace ncs::core::application
{

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

ServiceResult<NavigationResult>
NavigationService::routeToStation(const std::int64_t stationId,
                                  const std::optional<std::int64_t> latitudeE6,
                                  const std::optional<std::int64_t> longitudeE6,
                                  const std::string& keyword, const TravelMode mode)
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
    if (result.locationFallback)
    {
        result.origin = {StationService::defaultLatitudeE6, StationService::defaultLongitudeE6};
    }

    if (const auto route = routePlanner_.plan(result.origin, result.destination, mode))
    {
        result.distanceMeter = route->distanceMeter;
        result.durationSecond = route->durationSecond;
        result.polyline = route->polyline;
        result.steps = route->steps;
        return {core::domain::ErrorCode::Ok, std::move(result)};
    }

    result.routeFallback = true;
    result.distanceMeter = StationService::haversineMeter(
        result.origin.latitudeE6, result.origin.longitudeE6, result.destination.latitudeE6,
        result.destination.longitudeE6);
    result.polyline = {result.origin, result.destination};
    return {core::domain::ErrorCode::Ok, std::move(result)};
}

} // namespace ncs::core::application
