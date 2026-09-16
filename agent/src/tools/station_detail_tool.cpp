#include "agent/tools/station_detail_tool.h"

#include <string>
#include <vector>

namespace ncs::agent
{
namespace
{

const std::string schema = R"({
  "type": "object",
  "properties": {
    "stationId": {"type": "integer", "description": "站点 ID，来自 station_search 的结果"},
    "chargerType": {"type": "integer", "enum": [0, 1], "description": "0 慢充，1 快充"}
  },
  "required": ["stationId"]
})";

constexpr int chargerPageSize = 10;

std::string yuan(const int centPerKwh)
{
    return std::to_string(centPerKwh / 100) + "." + (centPerKwh % 100 < 10 ? "0" : "") +
           std::to_string(centPerKwh % 100);
}

std::string typesText(const std::vector<core::application::ChargerType>& types)
{
    if (types.empty())
        return "未知";
    std::string text;
    for (const auto type : types)
    {
        if (!text.empty())
            text += ",";
        text += core::application::chargerTypeName(type);
    }
    return text;
}

} // namespace

std::string_view StationDetailTool::name() const
{
    return "station_detail";
}

std::string_view StationDetailTool::description() const
{
    return "查询单个充电站的详情：价格构成、营业时间、设备数量、空闲数量和充电桩明细"
           "（快充/慢充、功率、接口标准、状态）。用户追问某个站点细节时使用。";
}

std::string_view StationDetailTool::parametersSchema() const
{
    return schema;
}

AgentToolResult StationDetailTool::invoke(const AgentContext& context, const QJsonObject& arguments)
{
    const auto stationId = integerArgument(arguments, "stationId", 1, 9007199254740991LL);
    if (!stationId)
        return AgentToolResult::failure(
            "station_detail requires a valid station id",
            "station_detail 调用缺少有效的 stationId，请先检索附近充电站。");

    auto chargerType = integerArgument(arguments, "chargerType", 0, 1);
    if (!chargerType && context.chargerType)
        chargerType = context.chargerType;

    const auto detail = stations_.stationDetail(*stationId, context.now);
    if (!detail.ok())
        return AgentToolResult::failure("station detail not found or tariff missing",
                                        "station_detail 未找到该站点，请重新检索附近充电站。");

    const auto chargers = stations_.stationChargers(
        *stationId,
        chargerType ? std::optional<core::application::ChargerType>(
                          static_cast<core::application::ChargerType>(*chargerType))
                    : std::nullopt,
        std::nullopt, 1, chargerPageSize);

    const auto& summary = detail.value->summary;
    std::string observation =
        "station_detail 站点详情：" + summary.name + "（id=" + std::to_string(summary.id) +
        "）地址=" + summary.address + " 营业时间=" + detail.value->businessHours +
        " 支持类型=" + typesText(detail.value->supportedTypes) +
        " 总价=" + yuan(summary.totalPriceCentPerKwh) + "元/千瓦时" +
        " 空闲=" + std::to_string(summary.idleCount) + "/" + std::to_string(summary.totalCount) +
        " 可用设备=" + std::to_string(summary.operationalCount);
    if (chargers.ok())
    {
        observation += " 充电桩明细=" + std::to_string(chargers.value->total) + " 条：";
        for (const auto& charger : chargers.value->items)
        {
            observation += "\n- " + charger.code + " " +
                           core::application::chargerTypeName(charger.type) + " " +
                           std::to_string(charger.powerWatt / 1000) + "千瓦 " +
                           charger.connectorStandard + " 状态=" + charger.statusText;
        }
    }

    AgentToolResult result;
    result.ok = true;
    result.stations.push_back(summary);
    result.observation = std::move(observation);
    result.payload = QJsonObject{
        {QStringLiteral("stationId"), static_cast<qint64>(summary.id)},
        {QStringLiteral("businessHours"), QString::fromStdString(detail.value->businessHours)},
        {QStringLiteral("chargerTotal"),
         static_cast<qint64>(chargers.ok() ? chargers.value->total : 0)},
    };
    return result;
}

} // namespace ncs::agent
