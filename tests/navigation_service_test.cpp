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
    int calls = 0;
    std::optional<ncs::core::application::RoutePoint> normalized;
    ncs::core::application::RoutePoint rawGps;
    std::optional<ncs::core::application::RoutePoint>
    normalizeGps(ncs::core::application::RoutePoint point) override
    {
        rawGps = point;
        return normalized;
    }
    ncs::core::application::RoutePoint lastOrigin;
    ncs::core::application::TravelMode lastMode = ncs::core::application::TravelMode::Driving;

    std::optional<ncs::core::application::PlannedRoute>
    plan(ncs::core::application::RoutePoint origin, ncs::core::application::RoutePoint,
         ncs::core::application::TravelMode mode) override
    {
        ++calls;
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
    const PlannedRoute valid{4200, 720, {{39908372, 116457658}, {39977680, 116316417}}, {}};
    const auto station = repository.station(1).value();
    const auto before = planner.calls;
    for (const auto offset : {0, 10})
    {
        planner.next = valid;
        const auto near = service.routeToStation(1, station.latitudeE6 + offset,
                                                 station.longitudeE6, "", TravelMode::Driving);
        if (!near.ok() || !near.value->routeFallback || near.value->distanceMeter > 5 ||
            planner.calls != before)
        {
            std::cerr << "FAIL: coincident or nearby origins must bypass external routing\n";
            return 1;
        }
    }
    for (const auto mode : {TravelMode::Driving, TravelMode::Walking, TravelMode::Transit})
    {
        planner.next = valid;
        const auto explicitOrigin = service.routeToStation(1, 39908372, 116457658, "ignored", mode);
        if (!explicitOrigin.ok() || explicitOrigin.value->routeFallback ||
            planner.lastOrigin.longitudeE6 != 116457658 || planner.lastMode != mode)
            return 1;
        for (int defect = 0; defect < 6; ++defect)
        {
            auto broken = valid;
            if (defect == 0)
                broken.distanceMeter = 1;
            if (defect == 1)
                broken.durationSecond = 0;
            if (defect == 2)
                broken.polyline.clear();
            if (defect == 3)
                broken.polyline.resize(1);
            if (defect == 4)
                broken.polyline[1] = broken.polyline[0];
            if (defect == 5)
                broken.polyline[1].latitudeE6 = 90000001;
            planner.next = broken;
            const auto result = service.routeToStation(1, 39908372, 116457658, "", mode);
            if (!result.ok() || !result.value->routeFallback || result.value->durationSecond != 0 ||
                result.value->polyline.size() != 2 || result.value->distanceMeter <= 1)
            {
                std::cerr
                    << "FAIL: degenerate provider success must become explicit local fallback\n";
                return 1;
            }
        }
    }
    planner.next = valid;
    planner.normalized = RoutePoint{39908372, 116457658};
    const auto gps = service.routeToStation(1, 39900000, 116450000, "", TravelMode::Driving, true);
    if (!gps.ok() || gps.value->routeFallback || gps.value->locationFallback ||
        gps.value->origin.latitudeE6 != 39908372 || planner.lastOrigin.longitudeE6 != 116457658 ||
        planner.rawGps.latitudeE6 != 39900000)
    {
        std::cerr << "FAIL: WGS84 must be normalized before route planning\n";
        return 1;
    }
    planner.normalized.reset();
    const auto gpsCalls = planner.calls;
    if (service.routeToStation(1, 39900000, 116450000, "", TravelMode::Driving, true).error !=
            ncs::core::domain::ErrorCode::ExternalServiceUnavailable ||
        planner.calls != gpsCalls)
    {
        std::cerr << "FAIL: failed GPS conversion must not use unconverted or invented origin\n";
        return 1;
    }
    return 0;
}
