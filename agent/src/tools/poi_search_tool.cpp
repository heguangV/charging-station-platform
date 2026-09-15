#include "agent/tools/poi_search_tool.h"

#include <string>
#include <vector>

namespace ncs::agent
{
namespace
{

const std::string schema = R"({
  "type": "object",
  "properties": {
    "category": {"type": "string", "enum": ["餐饮", "咖啡", "便利店", "商场"],
                 "description": "要检索的地点类别"},
    "keyword": {"type": "string", "description": "自定义检索关键词，优先级高于类别"},
    "latitudeE6": {"type": "integer", "description": "检索圆心纬度，1e-6 度整数，GCJ-02"},
    "longitudeE6": {"type": "integer", "description": "检索圆心经度，1e-6 度整数，GCJ-02"},
    "radiusMeter": {"type": "integer", "description": "检索半径，100-10000 米"},
    "limit": {"type": "integer", "description": "返回条数，1-10"}
  }
})";

std::string observationFor(const std::vector<core::application::PoiItem>& pois)
{
    std::string text = "poi_search 返回 " + std::to_string(pois.size()) + " 个地点：";
    for (std::size_t index = 0; index < pois.size(); ++index)
    {
        const auto& poi = pois[index];
        text += "\n" + std::to_string(index + 1) + ". " + poi.name + " 类别=" + poi.category +
                " 距离=" + std::to_string(poi.distanceMeter) + "米" + " 地址=" + poi.address;
    }
    return text;
}

} // namespace

std::string_view PoiSearchTool::name() const
{
    return "poi_search";
}

std::string_view PoiSearchTool::description() const
{
    return "检索充电站或用户位置周边的餐厅、咖啡店、便利店和商场，返回名称、类别与距离。"
           "用户问“附近哪里可以吃饭/喝咖啡/买东西”时使用。";
}

std::string_view PoiSearchTool::parametersSchema() const
{
    return schema;
}

AgentToolResult PoiSearchTool::invoke(const AgentContext& context, const QJsonObject& arguments)
{
    if (!provider_.available())
        return AgentToolResult::failure("poi provider is not configured",
                                        "poi_search 未配置地图服务，无法检索周边地点。");

    auto latitude = integerArgument(arguments, "latitudeE6", -90000000, 90000000);
    auto longitude = integerArgument(arguments, "longitudeE6", -180000000, 180000000);
    if (latitude.has_value() != longitude.has_value())
    {
        latitude.reset();
        longitude.reset();
    }
    // 优先使用调用方注入的锚点（通常是检索到的最近充电站），其次才用用户当前位置。
    if (!latitude && context.hasLocation())
    {
        latitude = context.latitudeE6;
        longitude = context.longitudeE6;
    }
    if (!latitude)
        return AgentToolResult::failure(
            "poi_search has no usable center coordinate",
            "poi_search 缺少可用的圆心坐标，请先获取用户位置或指定站点。");

    core::application::PoiQuery query;
    query.latitudeE6 = *latitude;
    query.longitudeE6 = *longitude;
    query.keyword = stringArgument(arguments, "keyword", 60);
    query.category = stringArgument(arguments, "category", 20);
    query.radiusMeter =
        static_cast<int>(integerArgument(arguments, "radiusMeter", 100, 10000).value_or(2000));
    query.limit = static_cast<int>(integerArgument(arguments, "limit", 1, 10).value_or(5));

    const auto result = provider_.search(query);
    if (!result.ok())
    {
        return AgentToolResult::failure("poi provider is unavailable",
                                        result.error == core::domain::ErrorCode::ValidationFailed
                                            ? "poi_search 圆心坐标不合法，无法检索周边地点。"
                                            : "poi_search 地图服务暂时不可用，未能检索周边地点。");
    }

    AgentToolResult toolResult;
    toolResult.ok = true;
    toolResult.pois = *result.value;
    toolResult.observation = toolResult.pois.empty()
                                 ? std::string("poi_search 查询成功，但圆心附近没有匹配的地点。")
                                 : observationFor(toolResult.pois);
    toolResult.payload = QJsonObject{
        {QStringLiteral("total"), static_cast<qint64>(toolResult.pois.size())},
        {QStringLiteral("radiusMeter"), query.radiusMeter},
    };
    return toolResult;
}

} // namespace ncs::agent
