#include "agent/agent_service.h"

#include "agent/agent_prompts.h"
#include "agent/tools/poi_search_tool.h"
#include "agent/tools/route_tool.h"
#include "agent/tools/station_detail_tool.h"
#include "agent/tools/station_search_tool.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <optional>
#include <utility>

namespace ncs::agent
{
namespace
{

constexpr std::size_t maxReplyLength = 400;

std::string trimReply(const std::string& value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    auto text = value.substr(first, last - first + 1);
    if (text.size() > maxReplyLength)
    {
        // 按 UTF-8 字符边界截断，避免产生半个多字节字符。
        auto cut = maxReplyLength;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
            --cut;
        text = text.substr(0, cut) + "…";
    }
    return text;
}

std::string yuan(const int centPerKwh)
{
    return std::to_string(centPerKwh / 100) + "." + (centPerKwh % 100 < 10 ? "0" : "") +
           std::to_string(centPerKwh % 100);
}

std::string distanceText(const std::int64_t meter)
{
    if (meter < 1000)
        return std::to_string(meter) + "米";
    // 展示层才换算单位：四舍五入到 0.1 公里。
    const auto hundredMeter = (meter + 50) / 100;
    return std::to_string(hundredMeter / 10) + "." + std::to_string(hundredMeter % 10) + "公里";
}

std::string durationText(const std::int64_t seconds)
{
    if (seconds <= 0)
        return "未知";
    const auto minutes = seconds / 60;
    if (minutes < 60)
        return std::to_string(minutes) + "分钟";
    return std::to_string(minutes / 60) + "小时" + std::to_string(minutes % 60) + "分钟";
}

bool containsAny(const std::string& message, const std::initializer_list<const char*> keys)
{
    return std::any_of(keys.begin(), keys.end(), [&message](const char* key)
                       { return message.find(key) != std::string::npos; });
}

// 确定性 POI 类别推断：LLM 不可用时也要按用户原话选择正确的检索类别，
// 否则“附近哪里能喝咖啡”会退化成默认的餐厅检索。
std::string poiCategoryFor(const std::string& message)
{
    if (containsAny(message, {"咖啡"}))
        return "咖啡";
    if (containsAny(message, {"吃", "餐", "饭", "美食"}))
        return "餐饮";
    if (containsAny(message, {"便利", "超市"}))
        return "便利店";
    if (containsAny(message, {"商场", "购物", "买"}))
        return "商场";
    return {};
}

// 依赖补齐：路线需要目的地坐标、站点详情需要站点 ID，模型通常不知道这些值。
// 因此当计划里出现这类工具却没有 station_search 时，先补一次站点检索建立锚点。
// POI 检索只在既没有用户定位、也没有显式圆心时才需要锚点。
bool requiresStationAnchor(const std::string& name, const QJsonObject& arguments,
                           const AgentContext& context)
{
    if (name == "route")
        return !arguments.contains(QStringLiteral("destinationLatitudeE6"));
    if (name == "station_detail")
        return !arguments.contains(QStringLiteral("stationId"));
    if (name == "poi_search")
        return !arguments.contains(QStringLiteral("latitudeE6")) && !context.hasLocation();
    return false;
}

} // namespace

AgentService::AgentService(core::application::StationService& stations,
                           core::application::RoutePlanner& routePlanner,
                           core::application::PoiProvider& pois, core::application::LlmClient& llm,
                           AgentLimits limits)
    : stations_(stations), pois_(pois), llm_(llm), limits_(limits), routeService_(routePlanner)
{
    // 注册顺序决定执行优先级；execute() 会按固定顺序重排，保证 station_search 先建立锚点。
    tools_.push_back(std::make_unique<StationSearchTool>(stations_));
    tools_.push_back(std::make_unique<StationDetailTool>(stations_));
    tools_.push_back(std::make_unique<PoiSearchTool>(pois_));
    tools_.push_back(std::make_unique<RouteTool>(routeService_));
}

std::vector<std::string> AgentService::toolNames() const
{
    std::vector<std::string> names;
    names.reserve(tools_.size());
    for (const auto& registered : tools_)
        names.emplace_back(registered->name());
    return names;
}

AgentTool* AgentService::tool(const std::string_view name) const
{
    for (const auto& registered : tools_)
    {
        if (registered->name() == name)
            return registered.get();
    }
    return nullptr;
}

std::vector<std::string> AgentService::planTools(const std::string& message) const
{
    // 确定性意图判断：LLM 不可用时的降级路径，也用于模型只回答不调用工具时补齐结构化数据。
    std::vector<std::string> planned{"station_search"};
    if (containsAny(message, {"导航", "怎么走", "路线", "多远", "多久", "开车", "步行", "公交",
                              "地铁", "过去", "到那里"}))
    {
        planned.emplace_back("route");
    }
    if (containsAny(message,
                    {"吃", "餐", "饭", "美食", "咖啡", "喝", "便利", "超市", "商场", "购物", "买"}))
    {
        planned.emplace_back("poi_search");
    }
    if (containsAny(message, {"详情", "营业", "几点", "充电桩", "设备", "功率", "接口", "快充桩",
                              "慢充桩", "多少桩", "价格构成"}))
    {
        planned.emplace_back("station_detail");
    }
    return planned;
}

std::vector<core::application::LlmToolSpec> AgentService::toolSpecs() const
{
    std::vector<core::application::LlmToolSpec> specs;
    specs.reserve(tools_.size());
    for (const auto& registered : tools_)
    {
        core::application::LlmToolSpec spec;
        spec.name = std::string(registered->name());
        spec.description = std::string(registered->description());
        spec.parametersJson = std::string(registered->parametersSchema());
        specs.push_back(std::move(spec));
    }
    return specs;
}

core::application::LlmMessage AgentService::systemMessage() const
{
    core::application::LlmMessage message;
    message.role = core::application::LlmRole::System;
    message.content = std::string(agentSystemPrompt());
    return message;
}

core::application::LlmMessage AgentService::userMessage(const std::string& message,
                                                        const AgentContext& context) const
{
    core::application::LlmMessage userMessageValue;
    userMessageValue.role = core::application::LlmRole::User;
    userMessageValue.content = message;
    userMessageValue.content += context.hasLocation() ? "\n（用户已授权浏览器定位，位置由系统提供，"
                                                        "无需再询问位置。）"
                                                      : "\n（用户未提供定位，如需精确结果可建议其"
                                                        "授权定位或直接给出地址。）";
    if (context.chargerType)
    {
        userMessageValue.content +=
            *context.chargerType == 1 ? "\n（用户偏好快充。）" : "\n（用户偏好慢充。）";
    }
    return userMessageValue;
}

std::vector<AgentService::Invocation> AgentService::planFromModel(const std::string& message,
                                                                  const AgentContext& context,
                                                                  bool& llmReachable)
{
    core::application::LlmChatRequest request;
    request.messages = {systemMessage(), userMessage(message, context)};
    request.tools = toolSpecs();

    const auto response = llm_.chat(request);
    if (!response.ok())
    {
        llmReachable = false;
        return {};
    }
    llmReachable = true;

    std::vector<Invocation> planned;
    for (const auto& call : response.value->toolCalls)
    {
        // 模型可能编造未注册的工具名：忽略即可，确定性兜底会补上必要的检索。
        if (tool(call.name) == nullptr)
            continue;
        Invocation invocation;
        invocation.name = call.name;
        const auto document =
            QJsonDocument::fromJson(QByteArray::fromStdString(call.argumentsJson));
        if (document.isObject())
            invocation.arguments = document.object();
        planned.push_back(std::move(invocation));
    }
    return planned;
}

void AgentService::execute(const std::vector<Invocation>& planned, const AgentContext& context,
                           AgentResult& result, std::vector<std::string>& observations,
                           bool& degraded)
{
    static const std::array<std::string_view, 4> executionOrder = {
        "station_search", "station_detail", "poi_search", "route"};

    std::vector<Invocation> ordered;
    std::vector<std::string> seen;
    for (const auto name : executionOrder)
    {
        for (const auto& invocation : planned)
        {
            if (invocation.name != name)
                continue;
            // 同名工具只执行一次，避免模型重复调用拖长响应时间。
            if (std::find(seen.begin(), seen.end(), name) != seen.end())
                continue;
            seen.emplace_back(name);
            ordered.push_back(invocation);
        }
    }

    std::optional<core::application::StationSummary> anchor;
    const auto executed = std::min(ordered.size(), limits_.maxToolCalls);
    for (std::size_t index = 0; index < executed; ++index)
    {
        const auto& invocation = ordered[index];
        auto* selected = tool(invocation.name);
        if (selected == nullptr)
            continue;

        auto arguments = invocation.arguments;
        if (anchor)
        {
            // 把 station_search 得到的最近站点作为锚点注入，使模型无需知道站点坐标。
            if (invocation.name == "poi_search")
            {
                if (!arguments.contains(QStringLiteral("latitudeE6")))
                {
                    arguments.insert(QStringLiteral("latitudeE6"),
                                     static_cast<qint64>(anchor->latitudeE6));
                    arguments.insert(QStringLiteral("longitudeE6"),
                                     static_cast<qint64>(anchor->longitudeE6));
                }
            }
            else if (invocation.name == "route")
            {
                if (!arguments.contains(QStringLiteral("destinationLatitudeE6")))
                {
                    arguments.insert(QStringLiteral("destinationLatitudeE6"),
                                     static_cast<qint64>(anchor->latitudeE6));
                    arguments.insert(QStringLiteral("destinationLongitudeE6"),
                                     static_cast<qint64>(anchor->longitudeE6));
                    arguments.insert(QStringLiteral("destinationName"),
                                     QString::fromStdString(anchor->name));
                }
            }
            else if (invocation.name == "station_detail")
            {
                if (!arguments.contains(QStringLiteral("stationId")))
                    arguments.insert(QStringLiteral("stationId"), static_cast<qint64>(anchor->id));
            }
        }

        const auto toolResult = selected->invoke(context, arguments);
        result.tools.push_back(invocation.name);
        if (!toolResult.ok)
        {
            // 工具失败只降级本次对话，不中断其余工具，也不把内部原因回给客户端。
            degraded = true;
            observations.push_back(toolResult.observation);
            continue;
        }
        observations.push_back(toolResult.observation);
        collectStructured(toolResult, result);
        if (invocation.name == "station_search" && !result.stations.empty())
            anchor = result.stations.front();
    }
}

void AgentService::collectStructured(const AgentToolResult& toolResult, AgentResult& result)
{
    for (const auto& station : toolResult.stations)
    {
        if (result.stations.size() >= limits_.maxStations)
            break;
        const auto duplicate =
            std::any_of(result.stations.begin(), result.stations.end(),
                        [&station](const core::application::StationSummary& existing)
                        { return existing.id == station.id; });
        if (!duplicate)
            result.stations.push_back(station);
    }
    for (const auto& poi : toolResult.pois)
    {
        if (result.pois.size() >= limits_.maxPois)
            break;
        result.pois.push_back(poi);
    }
    if (toolResult.route && !result.route)
        result.route = toolResult.route;
}

void AgentService::buildActions(AgentResult& result) const
{
    for (const auto& station : result.stations)
    {
        AgentAction action;
        action.type = "open_station";
        action.label = "查看 " + station.name;
        action.targetId = std::to_string(station.id);
        result.actions.push_back(std::move(action));
    }
    if (result.route)
    {
        AgentAction navigate;
        navigate.type = "navigate";
        navigate.label = "导航前往 " + result.route->destinationName;
        navigate.url = result.route->browserUrl;
        result.actions.push_back(std::move(navigate));
    }
}

AgentResult AgentService::chat(const std::string& message, const AgentContext& context)
{
    AgentResult result;
    std::vector<Invocation> planned;
    bool llmReachable = false;
    bool degraded = false;

    const bool llmEnabled = llm_.available();
    if (llmEnabled)
    {
        planned = planFromModel(message, context, llmReachable);
        if (!llmReachable)
            degraded = true;
    }
    if (planned.empty())
    {
        // 模型未选择工具、或 LLM 不可用：使用确定性意图判断，保证前端始终能拿到结构化数据。
        const auto category = poiCategoryFor(message);
        for (const auto& name : planTools(message))
        {
            Invocation invocation;
            invocation.name = name;
            if (name == "poi_search" && !category.empty())
                invocation.arguments.insert(QStringLiteral("category"),
                                            QString::fromStdString(category));
            planned.push_back(std::move(invocation));
        }
    }
    if (!llmEnabled)
        degraded = true;

    // 计划里的 route / station_detail / 无定位的 poi_search 需要锚点：若模型没有主动检索站点，
    // 这里补一次 station_search，保证工具拿到真实坐标与站点 ID 而不是失败。
    const bool needsAnchor = std::any_of(
        planned.begin(), planned.end(), [&context](const Invocation& invocation)
        { return requiresStationAnchor(invocation.name, invocation.arguments, context); });
    const bool hasSearch =
        std::any_of(planned.begin(), planned.end(), [](const Invocation& invocation)
                    { return invocation.name == "station_search"; });
    if (needsAnchor && !hasSearch)
    {
        Invocation anchor;
        anchor.name = "station_search";
        planned.insert(planned.begin(), std::move(anchor));
    }

    std::vector<std::string> observations;
    execute(planned, context, result, observations, degraded);
    buildActions(result);

    if (llmEnabled && llmReachable && !observations.empty())
    {
        core::application::LlmChatRequest request;
        request.messages = {systemMessage(), userMessage(message, context)};
        core::application::LlmMessage data;
        data.role = core::application::LlmRole::User;
        data.content = "以下是系统按用户问题查询到的真实数据，请只依据这些数据用简体中文回答：\n";
        for (const auto& observation : observations)
            data.content += observation + "\n";
        data.content += "如果数据为空，请如实说明并给出下一步建议；不要编造任何站点、地点或路线。";
        request.messages.push_back(std::move(data));
        const auto reply = llm_.chat(request);
        if (reply.ok())
        {
            auto text = trimReply(reply.value->content);
            if (!text.empty())
            {
                result.reply = std::move(text);
                result.llmUsed = true;
            }
        }
        if (!result.llmUsed)
            degraded = true;
    }

    if (!result.llmUsed)
    {
        // 确定性兜底文案：LLM 未配置、超时或返回空内容时仍然给出可用回答。
        std::string reply;
        if (!llmEnabled || !llmReachable)
            reply = std::string(agentLlmUnavailableNotice());
        if (!result.stations.empty())
        {
            if (!reply.empty())
                reply += " ";
            reply += "为你找到 " + std::to_string(result.stations.size()) + " 个充电站：";
            for (const auto& station : result.stations)
            {
                reply += "\n· " + station.name + "，" + distanceText(station.distanceMeter) +
                         "，总价 " + yuan(station.totalPriceCentPerKwh) + " 元/千瓦时，空闲 " +
                         std::to_string(station.idleCount) + "/" +
                         std::to_string(station.totalCount);
            }
        }
        else if (!planned.empty() && planned.front().name == "station_search")
        {
            if (!reply.empty())
                reply += " ";
            reply += std::string(agentNoStationNotice());
        }
        if (!result.pois.empty())
        {
            if (!reply.empty())
                reply += "\n";
            reply += "附近还有 " + std::to_string(result.pois.size()) + " 个可选地点：";
            for (const auto& poi : result.pois)
                reply += "\n· " + poi.name + "（" + poi.category + "）" +
                         distanceText(poi.distanceMeter);
        }
        else if (containsAny(message, {"吃", "餐", "饭", "咖啡", "便利", "商场", "购物"}))
        {
            reply += reply.empty() ? "" : "\n";
            reply += std::string(agentNoPoiNotice());
        }
        if (result.route)
        {
            if (!reply.empty())
                reply += "\n";
            reply += "到 " + result.route->destinationName + " 约 " +
                     distanceText(result.route->distanceMeter) + "，预计 " +
                     durationText(result.route->durationSecond) + "。";
            if (result.route->fallback)
                reply += " " + std::string(agentMapUnavailableNotice());
        }
        if (reply.empty())
            reply = std::string(agentLlmUnavailableNotice());
        result.reply = std::move(reply);
    }

    if (degraded && result.reply.find("地图服务暂时不可用") == std::string::npos && result.route &&
        result.route->fallback)
    {
        result.reply += " " + std::string(agentMapUnavailableNotice());
    }
    if (!context.hasLocation() && result.stations.empty() && !result.llmUsed)
        result.reply += " " + std::string(agentNoLocationNotice());

    result.degraded = degraded;
    return result;
}

} // namespace ncs::agent
