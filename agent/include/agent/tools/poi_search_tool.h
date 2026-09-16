// poi_search 工具：检索充电站或用户位置周边的餐厅、咖啡店、便利店和商场。
// 依赖 core::application::PoiProvider 端口（生产实现为 infrastructure/map 的腾讯地图
// WebService POI 服务），Agent 核心逻辑不感知具体地图厂商。
// 约束：缺少可用坐标、外部地图不可用或无结果都返回 ok=false，由 AgentService 降级为
// “附近暂无可推荐的 POI”说明，不得让整次对话失败。

#pragma once

#include "agent/agent_tool.h"
#include "core/application/poi_service.h"

#include <string_view>

namespace ncs::agent
{

class PoiSearchTool final : public AgentTool
{
  public:
    explicit PoiSearchTool(core::application::PoiProvider& provider) : provider_(provider) {}

    std::string_view name() const override;
    std::string_view description() const override;
    std::string_view parametersSchema() const override;
    AgentToolResult invoke(const AgentContext& context, const QJsonObject& arguments) override;

  private:
    core::application::PoiProvider& provider_;
};

} // namespace ncs::agent
