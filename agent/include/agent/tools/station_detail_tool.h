// station_detail 工具：读取单个站点的详情、价格、营业状态、设备统计与充电桩列表。
// 复用 core::application::StationService 的 stationDetail 与 stationChargers
// （即 GET /api/v1/user/stations/{stationId} 与 /chargers 的同一实现）。
// 约束：站点不存在返回 NotFound，工具以 ok=false 表达，由 AgentService 降级为文字说明。

#pragma once

#include "agent/agent_tool.h"
#include "core/application/station_service.h"

#include <string_view>

namespace ncs::agent
{

class StationDetailTool final : public AgentTool
{
  public:
    explicit StationDetailTool(core::application::StationService& stations) : stations_(stations) {}

    std::string_view name() const override;
    std::string_view description() const override;
    std::string_view parametersSchema() const override;
    AgentToolResult invoke(const AgentContext& context, const QJsonObject& arguments) override;

  private:
    core::application::StationService& stations_;
};

} // namespace ncs::agent
