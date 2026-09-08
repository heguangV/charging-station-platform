// ML worker 内部路由（/internal/ml/*）：按 MlTask 令牌 + MlWorker
// 角色鉴权，提供特征拉取、模型版本注册、批量预测回写与任务完成回调。 委托 MlService
// 校验任务能力（MlCapability）并落库，供 runtime 层拉起的 Python 子进程回调本服务。
#pragma once

#include "core/application/analytics_service.h"
#include "core/application/bounded_executor.h"
#include "core/application/idempotency_service.h"
#include "server/controller/api_routes.h"

namespace ncs::server::controller
{

class MlRoutes final
{
  public:
    MlRoutes(ApiRoutes& routes, core::application::MlService& ml,
             core::application::SessionManager& sessions,
             core::application::BoundedExecutor& executor,
             core::application::IdempotencyService& idempotency);
};

} // namespace ncs::server::controller
