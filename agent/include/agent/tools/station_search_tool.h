// station_search 工具：按用户位置、距离上限、充电类型、价格上限和空闲数量配额检索附近充电站。
// 直接委托 core::application::StationService（与 GET /api/v1/user/stations 同一实现与同一
// 降级规则），不新建任何独立的充电站数据逻辑。
// 约束：无定位时使用关键词地理编码，仍失败则由 StationService 回退默认坐标并置
// locationFallback，工具本身不得因此失败。

#pragma once

#include "agent/agent_tool.h"
#include "core/application/station_service.h"

#include <string_view>

namespace ncs::agent
{

class StationSearchTool final : public AgentTool
{
  public:
    explicit StationSearchTool(core::application::StationService& stations) : stations_(stations) {}

    std::string_view name() const override;
    std::string_view description() const override;
    std::string_view parametersSchema() const override;
    AgentToolResult invoke(const AgentContext& context, const QJsonObject& arguments) override;

  private:
    core::application::StationService& stations_;
};

} // namespace ncs::agent
