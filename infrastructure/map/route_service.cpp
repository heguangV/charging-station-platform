#include "infrastructure/map/route_service.h"

#include <optional>

namespace ncs::infrastructure::map
{

std::optional<RouteEstimate> RouteService::route(const core::application::RoutePoint origin,
                                                 const core::application::RoutePoint destination,
                                                 const core::application::TravelMode mode,
                                                 const bool wgs84Origin,
                                                 const std::string& destinationName) const
{
    RouteEstimate estimate;
    estimate.origin = origin;
    estimate.destination = destination;
    estimate.mode = mode;

    if (wgs84Origin)
    {
        const auto normalized = planner_.normalizeGps(origin);
        if (!normalized)
            return std::nullopt;
        estimate.origin = *normalized;
        estimate.coordinateNormalized = true;
    }

    const auto directDistance = core::application::StationService::haversineMeter(
        estimate.origin.latitudeE6, estimate.origin.longitudeE6, estimate.destination.latitudeE6,
        estimate.destination.longitudeE6);

    // 起终点重合时不去调用第三方：距离为 0、时长为 0，直接给出可用的兜底结果。
    if (directDistance > 5)
    {
        const auto planned = planner_.plan(estimate.origin, estimate.destination, mode);
        if (planned && core::application::usablePlannedRoute(*planned))
        {
            estimate.distanceMeter = planned->distanceMeter;
            estimate.durationSecond = planned->durationSecond;
            estimate.polyline = planned->polyline;
            estimate.steps = planned->steps;
        }
    }

    if (estimate.polyline.empty() || estimate.distanceMeter <= 1)
    {
        estimate.routeFallback = true;
        estimate.distanceMeter = directDistance;
        estimate.durationSecond = 0;
        estimate.polyline = {estimate.origin, estimate.destination};
        estimate.steps.clear();
    }

    estimate.browserUrl = core::application::browserRouteUrl(estimate.origin, estimate.destination,
                                                             destinationName, mode);
    return estimate;
}

} // namespace ncs::infrastructure::map
