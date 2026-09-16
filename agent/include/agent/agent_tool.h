// Agent 工具抽象：每个工具声明名称、给模型看的描述与参数 JSON Schema，并实现一次调用。
// AgentService 只通过本接口调度工具，因此新增能力不需要改动 Agent 核心逻辑，也不需要
// 让 Controller 感知任何编排细节。
// 约束：工具只能经由 core 应用服务端口（站点服务、地图端口）取数，不得直接访问 SQLite、
// 不得重新实现订单/充电/余额逻辑；工具失败必须返回 ok=false 并给出可回灌给模型的原因，
// 由 AgentService 决定降级，不允许抛异常穿透到 Crow 事件循环。

#pragma once

#include "agent/agent_context.h"
#include "agent/agent_result.h"
#include "core/application/llm_client.h"
#include "core/application/navigation_service.h"

#include <QJsonObject>
#include <QJsonValue>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ncs::agent
{

struct AgentToolResult
{
    bool ok = false;
    // 内部失败原因，仅用于日志与工具间编排，绝不直接进入客户端响应。
    std::string error;
    // 回灌给 LLM 的紧凑观察文本。
    std::string observation;
    // 结构化数据，供 AgentService 组装 AgentResult。
    QJsonObject payload;
    std::vector<core::application::StationSummary> stations;
    std::vector<AgentPoi> pois;
    std::optional<AgentRoute> route;

    static AgentToolResult failure(std::string reason, std::string observation)
    {
        AgentToolResult result;
        result.ok = false;
        result.error = std::move(reason);
        result.observation = std::move(observation);
        return result;
    }
};

class AgentTool
{
  public:
    virtual ~AgentTool() = default;

    virtual std::string_view name() const = 0;
    virtual std::string_view description() const = 0;
    // 参数 JSON Schema 对象字符串；必须可被 QJsonDocument 解析为对象。
    virtual std::string_view parametersSchema() const = 0;

    virtual AgentToolResult invoke(const AgentContext& context, const QJsonObject& arguments) = 0;
};

// —— 模型给出的参数一律视为不可信输入，必须做范围校验后才交给应用服务 ——
// 越界、类型不符或缺失统一返回 nullopt / 空串，由各工具决定如何使用默认值。

inline std::optional<std::int64_t> integerArgument(const QJsonObject& arguments, const char* key,
                                                   const std::int64_t minimum,
                                                   const std::int64_t maximum)
{
    const auto value = arguments.value(QLatin1String(key));
    if (value.isDouble())
    {
        const auto number = static_cast<std::int64_t>(value.toDouble());
        if (number >= minimum && number <= maximum)
            return number;
    }
    if (value.isString())
    {
        bool converted = false;
        const auto number = value.toString().toLongLong(&converted);
        if (converted && number >= minimum && number <= maximum)
            return number;
    }
    return std::nullopt;
}

inline std::string stringArgument(const QJsonObject& arguments, const char* key,
                                  const std::size_t maximumLength = 200)
{
    const auto value = arguments.value(QLatin1String(key));
    if (!value.isString())
        return {};
    const auto text = value.toString().trimmed();
    if (text.isEmpty() || static_cast<std::size_t>(text.size()) > maximumLength)
        return {};
    return text.toStdString();
}

inline bool booleanArgument(const QJsonObject& arguments, const char* key, const bool fallback)
{
    const auto value = arguments.value(QLatin1String(key));
    return value.isBool() ? value.toBool() : fallback;
}

inline std::optional<core::application::TravelMode> travelModeArgument(const QJsonObject& arguments,
                                                                       const char* key)
{
    const auto text = stringArgument(arguments, key, 16);
    if (text.empty())
        return std::nullopt;
    if (text == "driving")
        return core::application::TravelMode::Driving;
    if (text == "walking")
        return core::application::TravelMode::Walking;
    if (text == "transit")
        return core::application::TravelMode::Transit;
    return std::nullopt;
}

} // namespace ncs::agent
