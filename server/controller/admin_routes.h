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
