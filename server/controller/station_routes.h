// 用户端站点路由（/user/stations 及详情/充电桩/报价）：站点与充电桩查询、实时价格报价，委托
// StationService。 经 BoundedExecutor 异步执行并要求 User 会话鉴权。
#pragma once

#include "core/application/bounded_executor.h"
#include "core/application/session_manager.h"
#include "core/application/station_service.h"
#include "server/controller/api_routes.h"

namespace ncs::server::controller
{

class StationRoutes final
{
  public:
    StationRoutes(ApiRoutes& routes, core::application::StationService& stations,
                  core::application::SessionManager& sessions,
                  core::application::BoundedExecutor& executor);
};

} // namespace ncs::server::controller
