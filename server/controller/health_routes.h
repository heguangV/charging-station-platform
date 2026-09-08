// 探活路由：GET /system/health/live（进程存活）与 /system/health/ready（基于 ReadinessProbe
// 的就绪状态）。 最轻量的 controller，供负载均衡与运维检查使用。
#pragma once

#include "core/application/readiness_probe.h"
#include "core/application/session_manager.h"
#include "server/controller/api_routes.h"

namespace ncs::server::controller
{

class HealthRoutes final
{
  public:
    HealthRoutes(ApiRoutes& routes, core::application::ReadinessProbe& readinessProbe,
                 core::application::SessionManager& sessions);
};

} // namespace ncs::server::controller
