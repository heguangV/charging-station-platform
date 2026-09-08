// 导航应用服务：为指定充电站规划路线（驾车/步行/公交），返回距离、耗时、折线与分步指引。
// 依赖 ChargingRepository（站点坐标）与 Geocoder/RoutePlanner 端口（腾讯地图适配器）。
// 约束：外部地图失败必须降级——起点缺失回退默认坐标（locationFallback），路线失败回退 Haversine
// 直线（routeFallback），不得让请求失败。

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
    virtual std::optional<RoutePoint> normalizeGps(RoutePoint)
    {
        return std::nullopt;
    }
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

    // 规划到站路线：起点缺省时用关键词地理编码，仍缺失回退默认坐标（locationFallback=true）；外部路线规划失败回退
    // Haversine 直线与起终点折线（routeFallback=true），不使请求失败；站点不存在返回 NotFound。
    ServiceResult<NavigationResult> routeToStation(std::int64_t stationId,
                                                   std::optional<std::int64_t> latitudeE6,
                                                   std::optional<std::int64_t> longitudeE6,
                                                   const std::string& keyword, TravelMode mode,
                                                   bool gpsOrigin = false);

  private:
    ChargingRepository& repository_;
    Geocoder& geocoder_;
    RoutePlanner& routePlanner_;
};

std::string_view travelModeName(TravelMode mode);

} // namespace ncs::core::application
