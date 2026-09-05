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
