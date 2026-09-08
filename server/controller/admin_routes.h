// 管理端路由装配（/admin/auth、/admin/users、/admin/stations、/admin/chargers、/admin/tariffs、/admin/flows
// 等）。 位于 controller 层：校验入参并委托 Admin*Service 应用服务；写操作经 BoundedExecutor
// 异步执行，破坏性命令受 Idempotency-Key 幂等约束。
#pragma once

#include "core/application/admin_account_service.h"
#include "core/application/admin_auth_service.h"
#include "core/application/admin_ops_service.h"
#include "core/application/admin_station_service.h"
#include "core/application/admin_user_service.h"
#include "core/application/bounded_executor.h"
#include "core/application/idempotency_service.h"
#include "server/controller/api_routes.h"

namespace ncs::server::controller
{

class AdminRoutes final
{
  public:
    AdminRoutes(ApiRoutes& routes, core::application::AdminAuthService& auth,
                core::application::AdminUserService& users,
                core::application::AdminStationService& stations,
                core::application::AdminOpsService& ops,
                core::application::SessionManager& sessions,
                core::application::BoundedExecutor& executor,
                core::application::IdempotencyService& idempotency,
                core::application::AdminAccountService& accounts);
};

} // namespace ncs::server::controller
