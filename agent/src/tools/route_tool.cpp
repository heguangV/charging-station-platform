#include "agent/tools/route_tool.h"

#include <string>
#include <vector>

namespace ncs::agent
{
namespace
{

const std::string schema = R"({
  "type": "object",
  "properties": {
    "destinationLatitudeE6": {"type": "integer", "description": "目的地纬度，1e-6 度整数，GCJ-02"},
    "destinationLongitudeE6": {"type": "integer", "description": "目的地经度，1e-6 度整数，GCJ-02"},
    "destinationName": {"type": "string", "description": "目的地名称，用于导航链接与文案"},
    "originLatitudeE6": {"type": "integer", "description": "起点纬度，缺省使用用户当前位置"},
    "originLongitudeE6": {"type": "integer", "description": "起点经度，缺省使用用户当前位置"},
    "mode": {"type": "string", "enum": ["driving", "walking", "transit"],
             "description": "出行方式，缺省驾车"}
  },
  "required": ["destinationLatitudeE6", "destinationLongitudeE6"]
})";

std::string durationText(const std::int64_t seconds)
{
    if (seconds <= 0)
        return "未知";
    const auto minutes = seconds / 60;
    if (minutes < 60)
        return std::to_string(minutes) + "分钟";
    return std::to_string(minutes / 60) + "小时" + std::to_string(minutes % 60) + "分钟";
}

} // namespace

std::string_view RouteTool::name() const
{
    return "route";
}

std::string_view RouteTool::description() const
{
    return "计算从用户当前位置（或指定起点）到目的地的距离、路线与预计通行时间，"
           "并给出地图导航链接。用户问“怎么走/多远/要多久/导航过去”时使用。";
}

std::string_view RouteTool::parametersSchema() const
{
    return schema;
}

AgentToolResult RouteTool::invoke(const AgentContext& context, const QJsonObject& arguments)
{
    auto destinationLatitude =
        integerArgument(arguments, "destinationLatitudeE6", -90000000, 90000000);
    auto destinationLongitude =
        integerArgument(arguments, "destinationLongitudeE6", -180000000, 180000000);
    if (!destinationLatitude || !destinationLongitude)
    {
        return AgentToolResult::failure("route requires a destination coordinate",
                                        "route 缺少目的地坐标，请先检索附近充电站再规划路线。");
    }

    auto originLatitude = integerArgument(arguments, "originLatitudeE6", -90000000, 90000000);
    auto originLongitude = integerArgument(arguments, "originLongitudeE6", -180000000, 180000000);
    if (originLatitude.has_value() != originLongitude.has_value())
    {
        originLatitude.reset();
        originLongitude.reset();
    }
    if (!originLatitude && context.hasLocation())
    {
        originLatitude = context.latitudeE6;
        originLongitude = context.longitudeE6;
    }
    if (!originLatitude)
        return AgentToolResult::failure("route has no usable origin coordinate",
                                        "route 缺少可用起点，请先允许定位或提供出发地址。");

    const auto mode =
        travelModeArgument(arguments, "mode").value_or(core::application::TravelMode::Driving);
    const auto destinationName = stringArgument(arguments, "destinationName", 80);

    const auto estimate =
        routes_.route(core::application::RoutePoint{*originLatitude, *originLongitude},
                      core::application::RoutePoint{*destinationLatitude, *destinationLongitude},
                      mode, context.wgs84Location, destinationName);

    // 起点是浏览器 WGS84 且坐标转换失败时，明确返回失败：不使用默认坐标掩盖定位问题。
    if (!estimate)
        return AgentToolResult::failure(
            "route origin coordinate conversion failed",
            "route 无法把浏览器定位转换为地图坐标，请重新定位或提供地址。");

    AgentRoute route;
    route.destinationName = destinationName.empty() ? "目的地" : destinationName;
    route.originLatitudeE6 = estimate->origin.latitudeE6;
    route.originLongitudeE6 = estimate->origin.longitudeE6;
    route.destinationLatitudeE6 = estimate->destination.latitudeE6;
    route.destinationLongitudeE6 = estimate->destination.longitudeE6;
    route.distanceMeter = estimate->distanceMeter;
    route.durationSecond = estimate->durationSecond;
    route.provider = estimate->routeFallback ? "LOCAL_FALLBACK" : "TENCENT_MAP";
    route.fallback = estimate->routeFallback;
    route.polyline = estimate->polyline;
    route.browserUrl = estimate->browserUrl;
    for (const auto& step : estimate->steps)
        route.steps.push_back(
            AgentRouteStep{step.instruction, step.distanceMeter, step.durationSecond});

    std::string observation = "route " + std::string(core::application::travelModeName(mode)) +
                              " 到 " + route.destinationName +
                              "：距离=" + std::to_string(route.distanceMeter) +
                              "米，预计=" + durationText(route.durationSecond) +
                              (route.fallback ? "（地图服务不可用，为直线距离估算）" : "。");
    if (!route.steps.empty())
    {
        observation += " 路线要点：";
        for (const auto& step : route.steps)
        {
            observation +=
                "\n- " + step.instruction + " " + std::to_string(step.distanceMeter) + "米";
        }
    }

    AgentToolResult result;
    result.ok = true;
    result.route = std::move(route);
    result.observation = std::move(observation);
    result.payload = QJsonObject{
        {QStringLiteral("provider"), QString::fromStdString(result.route->provider)},
        {QStringLiteral("routeFallback"), result.route->fallback},
    };
    return result;
}

} // namespace ncs::agent
