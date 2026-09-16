// 地图路线服务：Agent 侧“距离 / 路线信息 / 预计通行信息”能力的唯一入口。
// 复用既有 core::application::RoutePlanner 端口（生产实现为腾讯地图 WebService 适配器），
// 在其之上补足坐标归一化与退化判定，使 Agent 的 route 工具不必自己编排地理编码、
// WGS84→GCJ-02 转换和降级语义。
// 约束：与前端腾讯地图 JavaScript API 分工明确——本服务只做服务端规划与降级计算，
// 不做地图渲染；外部规划失败必须回退 Haversine 直线并置 routeFallback，不得让请求失败。

#pragma once

#include "core/application/navigation_service.h"
#include "core/application/station_service.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ncs::infrastructure::map
{

struct RouteEstimate
{
    core::application::RoutePoint origin;
    core::application::RoutePoint destination;
    core::application::TravelMode mode = core::application::TravelMode::Driving;
    std::int64_t distanceMeter = 0;
    std::int64_t durationSecond = 0;
    std::vector<core::application::RoutePoint> polyline;
    std::vector<core::application::RouteStep> steps;
    // true 表示外部路线规划不可用，distanceMeter 为 Haversine 直线、durationSecond 为 0。
    bool routeFallback = false;
    // true 表示起点由 WGS84 归一化为 GCJ-02 后才参与规划。
    bool coordinateNormalized = false;
    // 腾讯地图浏览器导航链接，作为最终用户操作入口。
    std::string browserUrl;
};

class RouteService
{
  public:
    explicit RouteService(core::application::RoutePlanner& planner) : planner_(planner) {}

    // 起终点坐标必须是 GCJ-02；wgs84Origin 为 true 时先调用 RoutePlanner::normalizeGps
    // 转换，转换失败返回 std::nullopt（调用方提示重新定位），不使用默认坐标掩盖失败。
    std::optional<RouteEstimate> route(core::application::RoutePoint origin,
                                       core::application::RoutePoint destination,
                                       core::application::TravelMode mode, bool wgs84Origin,
                                       const std::string& destinationName) const;

  private:
    core::application::RoutePlanner& planner_;
};

} // namespace ncs::infrastructure::map
