#pragma once

#include "core/application/analytics_service.h"
#include "core/application/bounded_executor.h"
#include "core/application/idempotency_service.h"
#include "server/controller/api_routes.h"

namespace ncs::server::controller {

class MlRoutes final {
public:
  MlRoutes(ApiRoutes &routes, core::application::MlService &ml,
           core::application::SessionManager &sessions,
           core::application::BoundedExecutor &executor,
           core::application::IdempotencyService &idempotency);
};

} // namespace ncs::server::controller
