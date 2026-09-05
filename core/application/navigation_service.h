#pragma once

#include "core/application/charging_repository.h"
#include "core/application/service_result.h"
#include "core/application/station_service.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ncs::core::application
{

enum class TravelMode
{
    Driving,
    Walking,
    Transit
};

struct RoutePoint
{
    std::int64_t latitudeE6 = 0;
    std::int64_t longitudeE6 = 0;
};

struct RouteStep
{
    std::string instruction;
    std::int64_t distanceMeter = 0;
    std::int64_t durationSecond = 0;
};

struct PlannedRoute
{
    std::int64_t distanceMeter = 0;
    std::int64_t durationSecond = 0;
    std::vector<RoutePoint> polyline;
    std::vector<RouteStep> steps;
};

class RoutePlanner
{
  public:
    virtual ~RoutePlanner() = default;
    virtual std::optional<PlannedRoute> plan(RoutePoint origin, RoutePoint destination,
                                             TravelMode mode) = 0;
};

struct NavigationResult
{
    std::int64_t stationId = 0;
    std::string stationName;
    std::string destinationAddress;
    RoutePoint origin;
    RoutePoint destination;
    TravelMode mode = TravelMode::Driving;
    std::int64_t distanceMeter = 0;
    std::int64_t durationSecond = 0;
    std::vector<RoutePoint> polyline;
    std::vector<RouteStep> steps;
    bool locationFallback = false;
    bool routeFallback = false;
};

class NavigationService final
{
  public:
    NavigationService(ChargingRepository& repository, Geocoder& geocoder,
                      RoutePlanner& routePlanner)
        : repository_(repository), geocoder_(geocoder), routePlanner_(routePlanner)
    {
    }

    ServiceResult<NavigationResult> routeToStation(std::int64_t stationId,
                                                   std::optional<std::int64_t> latitudeE6,
                                                   std::optional<std::int64_t> longitudeE6,
                                                   const std::string& keyword, TravelMode mode);

  private:
    ChargingRepository& repository_;
    Geocoder& geocoder_;
    RoutePlanner& routePlanner_;
};

std::string_view travelModeName(TravelMode mode);

} // namespace ncs::core::application
