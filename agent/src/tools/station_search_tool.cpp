#include "agent/tools/station_search_tool.h"

#include <algorithm>
#include <string>
#include <vector>

namespace ncs::agent
{
namespace
{

const std::string schema = R"({
  "type": "object",
  "properties": {
    "keyword": {"type": "string", "description": "用户位置地址或站点名称关键词，用户只说地址时使用"},
    "latitudeE6": {"type": "integer", "description": "用户位置纬度，1e-6 度整数，GCJ-02"},
    "longitudeE6": {"type": "integer", "description": "用户位置经度，1e-6 度整数，GCJ-02"},
    "chargerType": {"type": "integer", "enum": [0, 1], "description": "0 慢充，1 快充"},
    "maxDistanceMeter": {"type": "integer", "description": "距离上限，单位米"},
    "maxTotalPriceCentPerKwh": {"type": "integer", "description": "总价上限，单位分/千瓦时"},
    "minIdleCount": {"type": "integer", "description": "至少需要的空闲充电桩数量"},
    "limit": {"type": "integer", "description": "返回条数，1-10"}
  }
})";

std::string yuan(const int centPerKwh)
{
    return std::to_string(centPerKwh / 100) + "." + (centPerKwh % 100 < 10 ? "0" : "") +
           std::to_string(centPerKwh % 100);
}

std::string observationFor(const std::vector<core::application::StationSummary>& stations)
{
    std::string text = "station_search 返回 " + std::to_string(stations.size()) + " 个站点：";
    for (std::size_t index = 0; index < stations.size(); ++index)
    {
        const auto& station = stations[index];
        text += "\n" + std::to_string(index + 1) + ". " + station.name +
                "（id=" + std::to_string(station.id) + "）地址=" + station.address +
                " 距离=" + std::to_string(station.distanceMeter) + "米" +
                " 总价=" + yuan(station.totalPriceCentPerKwh) + "元/千瓦时" + "（电费 " +
                yuan(station.electricityPriceCentPerKwh) + " + 服务费 " +
                yuan(station.servicePriceCentPerKwh) + "）" +
                " 空闲=" + std::to_string(station.idleCount) + "/" +
                std::to_string(station.totalCount);
    }
    return text;
}

} // namespace

std::string_view StationSearchTool::name() const
{
    return "station_search";
}

std::string_view StationSearchTool::description() const
{
    return "按用户位置、距离、充电类型、价格和空闲数量检索附近充电站，返回站点列表与距离。"
           "用户询问附近充电站、找快充、便宜站点或“哪里能充电”时使用。";
}

std::string_view StationSearchTool::parametersSchema() const
{
    return schema;
}

AgentToolResult StationSearchTool::invoke(const AgentContext& context, const QJsonObject& arguments)
{
    using core::application::ChargerType;
    using core::application::StationSummary;

    auto latitude = integerArgument(arguments, "latitudeE6", -90000000, 90000000);
    auto longitude = integerArgument(arguments, "longitudeE6", -180000000, 180000000);
    if (latitude.has_value() != longitude.has_value())
    {
        latitude.reset();
        longitude.reset();
    }
    // 模型没给坐标时使用会话上下文中的浏览器定位；两者都缺失时交给 StationService
    // 走关键词地理编码与默认坐标降级，而不是在这里判定失败。
    if (!latitude && context.hasLocation())
    {
        latitude = context.latitudeE6;
        longitude = context.longitudeE6;
    }

    auto chargerType = integerArgument(arguments, "chargerType", 0, 1);
    if (!chargerType && context.chargerType)
        chargerType = context.chargerType;

    const auto keyword = stringArgument(arguments, "keyword", 200);
    const auto limit = integerArgument(arguments, "limit", 1, 10).value_or(5);
    const auto maxDistance = integerArgument(arguments, "maxDistanceMeter", 1, 500000).value_or(0);
    const auto maxPrice =
        integerArgument(arguments, "maxTotalPriceCentPerKwh", 1, 100000).value_or(0);
    const auto minIdle = integerArgument(arguments, "minIdleCount", 0, 100).value_or(0);

    const auto result = stations_.nearbyStations(
        latitude, longitude, keyword,
        chargerType ? std::optional<ChargerType>(static_cast<ChargerType>(*chargerType))
                    : std::nullopt,
        1, static_cast<int>(limit), context.now);

    std::vector<StationSummary> filtered;
    for (const auto& station : result.items)
    {
        if (maxDistance > 0 && station.distanceMeter > maxDistance)
            continue;
        if (maxPrice > 0 && station.totalPriceCentPerKwh > maxPrice)
            continue;
        if (minIdle > 0 && station.idleCount < minIdle)
            continue;
        filtered.push_back(station);
    }

    AgentToolResult toolResult;
    toolResult.ok = true;
    toolResult.stations = filtered;
    // 过滤后为空也保持 ok=true：这是“查询成功但没有匹配结果”，由 AgentService 决定文案。
    toolResult.observation =
        filtered.empty() ? std::string("station_search 查询成功，但没有符合筛选条件的站点。")
                         : observationFor(filtered);
    toolResult.payload = QJsonObject{
        {QStringLiteral("total"), static_cast<qint64>(filtered.size())},
        {QStringLiteral("locationFallback"), result.locationFallback},
        {QStringLiteral("usedKeyword"), QString::fromStdString(keyword)},
    };
    return toolResult;
}

} // namespace ncs::agent
