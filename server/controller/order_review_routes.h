#pragma once

#include "core/application/bounded_executor.h"
#include "core/application/idempotency_service.h"
#include "core/application/order_review_service.h"
#include "core/application/session_manager.h"
#include "server/controller/api_routes.h"

namespace ncs::server::controller
{
class OrderReviewRoutes final
{
  public:
    OrderReviewRoutes(ApiRoutes& routes, core::application::OrderReviewService& reviews,
                      core::application::SessionManager& sessions,
                      core::application::BoundedExecutor& executor,
                      core::application::IdempotencyService& idempotency);
};
} // namespace ncs::server::controller
