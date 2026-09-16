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

// 退化路线判定：距离 ≤1 米、时长 ≤0、折线不足两个点、坐标越界或折线全部重合时视为无效，
// 调用方必须回退本地 Haversine 直线而不是把第三方 status=0 当作成功路线。
// 用户端导航服务与地图路线服务共用同一判定，避免两处各写一套退化检查。
bool usablePlannedRoute(const PlannedRoute& route);

// 腾讯地图浏览器路线规划链接（apis.map.qq.com/uri/v1/routeplan），作为最终用户操作入口。
// 用户端导航接口与 Agent 路线推荐共用同一构造，避免两处各写一份 URL 拼装。
std::string browserRouteUrl(RoutePoint origin, RoutePoint destination,
                            const std::string& destinationName, TravelMode mode);

} // namespace ncs::core::application
