#pragma once

#include "core/application/admin_ops_service.h"
#include "core/application/analytics_service.h"
#include "core/application/session_manager.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace ncs::server::runtime {

struct MlProcessOptions {
  std::string pythonExecutable;
  std::string workerScript;
  std::string baseUrl;
  std::string caCertificatePath;
  std::string modelPath;
};

class MlProcessManager final : public core::application::MlTaskLauncher {
public:
  MlProcessManager(MlProcessOptions options,
                   core::application::SessionManager &sessions,
                   core::application::AdminRepository &admin,
                   core::application::AnalyticsRepository &analytics,
                   core::application::ModelArtifactStore *artifacts = nullptr);
  ~MlProcessManager() override;

  bool start(const core::application::MlTask &task) override;
  void stop(std::string_view taskNo) override;

private:
  struct Worker {
    std::atomic_bool stopRequested{false};
    std::atomic_bool finished{false};
    std::thread thread;
  };

  void reapFinished();
  MlProcessOptions options_;
  core::application::SessionManager &sessions_;
  core::application::AdminRepository &admin_;
  core::application::AnalyticsRepository &analytics_;
  core::application::ModelArtifactStore *artifacts_ = nullptr;
  std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<Worker>> workers_;
};

} // namespace ncs::server::runtime
