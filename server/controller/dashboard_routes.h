// 管理端仪表盘路由：把营收/排名/热力/预测等聚合统计组织为 JSON 快照，经 AtomicSnapshotWriter
// 原子落盘并经 EventHub 广播刷新。 refreshAndExport 在启动时预生成一次，之后按 30
// 秒周期刷新，失败时把快照标记为 stale。
#pragma once

#include "core/application/admin_auth_service.h"
#include "core/application/analytics_service.h"
#include "core/application/bounded_executor.h"
#include "core/application/event_hub.h"
#include "infrastructure/files/atomic_snapshot_writer.h"
#include "server/controller/api_routes.h"

#include <QJsonObject>
#include <chrono>

namespace ncs::server::controller
{

QJsonObject dashboardSnapshotJson(const core::application::DashboardSnapshot& snapshot);

class DashboardRoutes final
{
  public:
    DashboardRoutes(ApiRoutes& routes, core::application::AdminAuthService& auth,
                    core::application::DashboardService& dashboard,
                    core::application::SessionManager& sessions,
                    core::application::BoundedExecutor& executor, std::string snapshotPath,
                    std::shared_ptr<core::application::EventHub> hub = {});

    bool refreshAndExport(std::chrono::system_clock::time_point now);

  private:
    core::application::DashboardService& dashboard_;
    infrastructure::files::AtomicSnapshotWriter writer_;
    std::shared_ptr<core::application::EventHub> hub_;
};

} // namespace ncs::server::controller
