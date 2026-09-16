// AgentService：Agent 模块的唯一编排核心，负责
//   理解自然语言 → 判断意图 → 选择工具 → 调用一个或多个工具 → 汇总工具结果 →
//   调用 LLM 生成自然语言 → 返回「自然语言 + 结构化数据」。
// 依赖方向为 agent → core / infrastructure：站点能力复用 core 应用服务，地图与 LLM 通过
// core 端口注入，Agent 不直接访问 SQLite，不重新实现订单/充电/余额逻辑，也不修改任何业务状态。
// 约束：LLM 未配置、调用失败或超时必须走确定性兜底路径并置 degraded=true，绝不返回空回复；
// 工具调用次数与轮次有上限，避免模型自激循环占用有界阻塞工作队列。

#pragma once

#include "agent/agent_context.h"
#include "agent/agent_result.h"
#include "agent/agent_tool.h"
#include "core/application/llm_client.h"
#include "core/application/navigation_service.h"
#include "core/application/poi_service.h"
#include "core/application/station_service.h"
#include "infrastructure/map/route_service.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ncs::agent
{

struct AgentLimits
{
    // 单次对话最多执行的工具调用数（超出部分被忽略，不视为失败）。
    std::size_t maxToolCalls = 4;
    // 结构化响应中最多返回的站点数。
    std::size_t maxStations = 3;
    // 结构化响应中最多返回的 POI 数。
    std::size_t maxPois = 5;
    // POI 检索半径（米）。
    int poiRadiusMeter = 2000;
};

class AgentService
{
  public:
    AgentService(core::application::StationService& stations,
                 core::application::RoutePlanner& routePlanner,
                 core::application::PoiProvider& pois, core::application::LlmClient& llm,
                 AgentLimits limits = {});

    // 一次对话。message 已由 Controller 做过长度与非空校验；context.userId 来自会话令牌。
    // 任何内部失败都不会抛出，最差返回带说明文字与 degraded=true 的结果。
    AgentResult chat(const std::string& message, const AgentContext& context);

    // 已注册工具名，按注册顺序。
    std::vector<std::string> toolNames() const;

    // 按名查找工具；未注册返回 nullptr。供测试验证工具调度。
    AgentTool* tool(std::string_view name) const;

    // 确定性工具选择（不依赖 LLM）：用于 LLM 不可用时的降级意图判断，也可离线断言。
    // 至少返回一个工具，缺省为 station_search。
    std::vector<std::string> planTools(const std::string& message) const;

  private:
    struct Invocation
    {
        std::string name;
        QJsonObject arguments;
    };

    std::vector<Invocation> planFromModel(const std::string& message, const AgentContext& context,
                                          bool& llmReachable);
    std::vector<core::application::LlmToolSpec> toolSpecs() const;
    core::application::LlmMessage systemMessage() const;
    core::application::LlmMessage userMessage(const std::string& message,
                                              const AgentContext& context) const;

    // 顺序执行计划：station_search 先跑以获得锚点站点，随后 poi_search / route 可继承锚点坐标。
    void execute(const std::vector<Invocation>& planned, const AgentContext& context,
                 AgentResult& result, std::vector<std::string>& observations, bool& degraded);
    void collectStructured(const AgentToolResult& toolResult, AgentResult& result);
    void buildActions(AgentResult& result) const;

    core::application::StationService& stations_;
    core::application::PoiProvider& pois_;
    core::application::LlmClient& llm_;
    AgentLimits limits_;
    // 必须在 tools_ 之前声明：各工具持有对本成员的引用。
    infrastructure::map::RouteService routeService_;
    std::vector<std::unique_ptr<AgentTool>> tools_;
};

} // namespace ncs::agent
