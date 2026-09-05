#pragma once

#include "core/application/bounded_executor.h"
#include "core/application/session_manager.h"
#include "core/application/user_identity_service.h"
#include "server/controller/api_routes.h"

namespace ncs::server::controller {

class UserIdentityRoutes final {
public:
    UserIdentityRoutes(
        ApiRoutes &routes,
        core::application::UserIdentityService &identity,
        core::application::SessionManager &sessions,
        core::application::BoundedExecutor &blockingExecutor,
        bool developmentMode);
};

} // namespace ncs::server::controller
