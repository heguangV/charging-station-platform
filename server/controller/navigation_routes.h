// 导航路由（/user/stations/<id>/route）：委托 NavigationService
// 调用腾讯地理编码与路线规划并组织返回结果。 外部地图调用较慢，经 BoundedExecutor 异步执行并要求
// User 会话鉴权。
#pragma once

#include "core/application/bounded_executor.h"
#include "core/application/navigation_service.h"
#include "core/application/session_manager.h"
#include "server/controller/api_routes.h"

namespace ncs::server::controller
{

class NavigationRoutes final
{
  public:
    NavigationRoutes(ApiRoutes& routes, core::application::NavigationService& navigation,
                     core::application::SessionManager& sessions,
                     core::application::BoundedExecutor& executor);
};

} // namespace ncs::server::controller
