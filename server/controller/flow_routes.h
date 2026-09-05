#pragma once

#include "core/application/bounded_executor.h"
#include "core/application/charge_flow_service.h"
#include "core/application/idempotency_service.h"
#include "core/application/session_manager.h"
#include "server/controller/api_routes.h"

namespace ncs::server::controller {

class FlowRoutes final {
public:
    FlowRoutes(
        ApiRoutes &routes,
        core::application::ChargeFlowService &flows,
        core::application::SessionManager &sessions,
        core::application::BoundedExecutor &executor,
        core::application::IdempotencyService &idempotency);
};

} // namespace ncs::server::controller
