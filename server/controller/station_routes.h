#pragma once

#include "core/application/bounded_executor.h"
#include "core/application/session_manager.h"
#include "core/application/station_service.h"
#include "server/controller/api_routes.h"

namespace ncs::server::controller {

class StationRoutes final {
public:
    StationRoutes(
        ApiRoutes &routes,
        core::application::StationService &stations,
        core::application::SessionManager &sessions,
        core::application::BoundedExecutor &executor);
};

} // namespace ncs::server::controller
