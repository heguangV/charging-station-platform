#pragma once

#include "core/application/admin_auth_service.h"
#include "core/application/analytics_service.h"
#include "core/application/bounded_executor.h"
#include "core/application/event_hub.h"
#include "infrastructure/files/atomic_snapshot_writer.h"
#include "server/controller/api_routes.h"

#include <chrono>
#include <QJsonObject>

namespace ncs::server::controller {

QJsonObject dashboardSnapshotJson(
    const core::application::DashboardSnapshot &snapshot);

class DashboardRoutes final {
public:
  DashboardRoutes(ApiRoutes &routes, core::application::AdminAuthService &auth,
                  core::application::DashboardService &dashboard,
                  core::application::SessionManager &sessions,
                  core::application::BoundedExecutor &executor,
                  std::string snapshotPath,
                  std::shared_ptr<core::application::EventHub> hub = {});

  bool refreshAndExport(std::chrono::system_clock::time_point now);

private:
  core::application::DashboardService &dashboard_;
  infrastructure::files::AtomicSnapshotWriter writer_;
  std::shared_ptr<core::application::EventHub> hub_;
};

} // namespace ncs::server::controller
