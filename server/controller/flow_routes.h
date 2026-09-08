// 用户充电流程路由（/user/flows、/user/orders）：报价确认、启动、取消、进度、结算等命令与查询，委托
// ChargeFlowService。 写命令经 BoundedExecutor 异步执行，并按 Idempotency-Key 契约保证重试安全。
#pragma once

#include "core/application/bounded_executor.h"
#include "core/application/charge_flow_service.h"
#include "core/application/idempotency_service.h"
#include "core/application/session_manager.h"
#include "server/controller/api_routes.h"

namespace ncs::server::controller
{

class FlowRoutes final
{
  public:
    FlowRoutes(ApiRoutes& routes, core::application::ChargeFlowService& flows,
               core::application::SessionManager& sessions,
               core::application::BoundedExecutor& executor,
               core::application::IdempotencyService& idempotency);
};

} // namespace ncs::server::controller
