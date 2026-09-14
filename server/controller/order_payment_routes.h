#pragma once
#include "server/controller/flow_routes.h"
namespace ncs::server::controller {
void registerOrderPaymentRoutes(ApiRoutes& routes, core::application::ChargeFlowService& flows,
    core::application::SessionManager& sessions, core::application::BoundedExecutor& executor,
    core::application::IdempotencyService& idempotency);
}
