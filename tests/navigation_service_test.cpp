#include "core/application/charging_repository.h"
#include "core/application/navigation_service.h"

#include <iostream>
#include <optional>
#include <string>

namespace
{

class FixedGeocoder final : public ncs::core::application::Geocoder
{
  public:
    explicit FixedGeocoder(std::optional<Location> location) : location_(location) {}
    std::optional<Location> resolve(const std::string&) override
    {
        return location_;
    }

  private:
    std::optional<Location> location_;
};

class FixedPlanner final : public ncs::core::application::RoutePlanner
{
  public:
    std::optional<ncs::core::application::PlannedRoute> next;
    ncs::core::application::RoutePoint lastOrigin;
    ncs::core::application::TravelMode lastMode = ncs::core::application::TravelMode::Driving;

    std::optional<ncs::core::application::PlannedRoute>
    plan(ncs::core::application::RoutePoint origin, ncs::core::application::RoutePoint,
         ncs::core::application::TravelMode mode) override
    {
        lastOrigin = origin;
        lastMode = mode;
        return next;
    }
};

} // namespace

int main()
{
    using namespace ncs::core::application;
    InMemoryChargingRepository repository;
    FixedGeocoder geocoder(Geocoder::Location{39908372, 116457658});
    FixedPlanner planner;
    planner.next = PlannedRoute{
        4200,
        720,
        {{39908372, 116457658}, {39977680, 116316417}},
        {{"向东行驶", 300, 60}},
    };
    NavigationService service(repository, geocoder, planner);

    const auto planned =
        service.routeToStation(1, std::nullopt, std::nullopt, "北京市朝阳区", TravelMode::Transit);
    if (!planned.ok() || planned.value->locationFallback || planned.value->routeFallback ||
        planned.value->distanceMeter != 4200 || planned.value->durationSecond != 720 ||
        planner.lastOrigin.latitudeE6 != 39908372 || planner.lastMode != TravelMode::Transit)
    {
        std::cerr << "FAIL: Tencent route data must remain the primary result\n";
        return 1;
    }

    planner.next = std::nullopt;
    const auto fallback =
        service.routeToStation(1, std::nullopt, std::nullopt, "", TravelMode::Walking);
    if (!fallback.ok() || !fallback.value->locationFallback || !fallback.value->routeFallback ||
        fallback.value->distanceMeter < 0 ||
        fallback.value->origin.latitudeE6 != StationService::defaultLatitudeE6 ||
        fallback.value->polyline.size() != 2)
    {
        std::cerr << "FAIL: map failure must use default origin and local route\n";
        return 1;
    }

    if (service.routeToStation(999, 39900000, 116300000, "", TravelMode::Driving).error !=
        ncs::core::domain::ErrorCode::NotFound)
    {
        std::cerr << "FAIL: missing station must not call the external planner\n";
        return 1;
    }
    return 0;
}
