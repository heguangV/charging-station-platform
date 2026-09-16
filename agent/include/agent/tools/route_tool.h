// route 工具：给出到目标地点的距离、路线信息与预计通行信息。
// 依赖 infrastructure/map 的 RouteService，它组合既有 RoutePlanner 端口完成腾讯地图
// WebService 规划、WGS84→GCJ-02 归一化与 Haversine 降级。
// 约束：目标坐标缺失或起终点不完整时返回 ok=false；外部规划失败时仍以 routeFallback
// 结果返回 ok=true，由前端明确标注“直线距离估算”。

#pragma once

#include "agent/agent_tool.h"
#include "infrastructure/map/route_service.h"

#include <string_view>

namespace ncs::agent
{

class RouteTool final : public AgentTool
{
  public:
    explicit RouteTool(infrastructure::map::RouteService& routes) : routes_(routes) {}

    std::string_view name() const override;
    std::string_view description() const override;
    std::string_view parametersSchema() const override;
    AgentToolResult invoke(const AgentContext& context, const QJsonObject& arguments) override;

  private:
    infrastructure::map::RouteService& routes_;
};

} // namespace ncs::agent
