#pragma once

#include "core/application/readiness_probe.h"
#include "core/application/session_manager.h"
#include "server/controller/api_routes.h"

namespace ncs::server::controller {

class HealthRoutes final {
public:
    HealthRoutes(
        ApiRoutes &routes,
        core::application::ReadinessProbe &readinessProbe,
        core::application::SessionManager &sessions);
};

} // namespace ncs::server::controller
