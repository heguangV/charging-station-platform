#pragma once

#include "core/application/admin_repository.h"
#include "core/application/analytics_service.h"
#include "core/application/business_numbers.h"
#include "core/application/charge_flow_service.h"
#include "core/application/service_result.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <mutex>
#include <string>
#include <vector>

namespace ncs::core::application {

struct RevenueBucket {
  std::int64_t bucketStart = 0;
  std::int64_t amountCent = 0;
  std::int64_t energyMwh = 0;
  int orderCount = 0;
};

struct RevenueStats {
  std::vector<RevenueBucket> items;
  std::int64_t totalAmountCent = 0;
  std::int64_t totalEnergyMwh = 0;
  int totalOrderCount = 0;
};

struct ChargerStatusStats {
  int idleCount = 0;
  int occupiedCount = 0;
  int faultyCount = 0;
  int restartingCount = 0;
  int disabledCount = 0;
  int operationalCount = 0;
  int totalCount = 0;
  int healthPercent = 0;
};

struct PredictionView {
  std::int64_t stationId = 0;
  int horizonHour = 1;
  std::string modelVersion;
  std::int64_t generatedAt = 0;
  std::int64_t targetAt = 0;
  std::int64_t predictedEnergyMwh = 0;
  int predictedIdleCount = 0;
  bool peakFlag = false;
  bool staleFlag = false;
};

class MlTaskLauncher {
public:
  virtual ~MlTaskLauncher() = default;
  virtual bool start(const MlTask &task) = 0;
  virtual void stop(std::string_view taskNo) = 0;
};

// Section 6.3/6.4 admin operations: flow oversight, force release, statistics,
// audit queries, backup orchestration records and ML task bookkeeping.
class AdminOpsService final {
public:
  AdminOpsService(AdminRepository &repository, ChargingRepository &charging,
                  ChargeFlowService &flows, BusinessNumbers &numbers,
                  AnalyticsRepository *analytics = nullptr,
                  MlTaskLauncher *launcher = nullptr)
      : repository_(repository), charging_(charging), flows_(flows),
        numbers_(numbers), analytics_(analytics), launcher_(launcher) {}

  AdminFlowPage flows(const AdminFlowQuery &query);
  ServiceResult<FlowView>
  forceRelease(std::int64_t actorAdminId, const std::string &flowNo,
               const std::string &reason, int nextChargerStatus,
               std::int64_t flowVersion,
               std::chrono::system_clock::time_point now);

  ServiceResult<RevenueStats> revenueStats(std::int64_t fromAt, std::int64_t toAt,
                                           std::optional<std::int64_t> stationId,
                                           const std::string &bucket,
                                           std::chrono::system_clock::time_point now =
                                               std::chrono::system_clock::now());
  ChargerStatusStats chargerStatusStats(std::optional<std::int64_t> stationId);

  std::vector<AuditEvent> auditEvents(const AuditEventQuery &query);

  ServiceResult<BackupRecord>
  createBackup(std::int64_t actorAdminId,
               std::chrono::system_clock::time_point now);
  std::vector<BackupRecord> backups();
  ServiceResult<BackupRecord>
  verifyBackup(std::int64_t actorAdminId, const std::string &backupNo,
               std::chrono::system_clock::time_point now);

  ServiceResult<MlTask> startMlTask(std::int64_t actorAdminId,
                                    const std::string &taskType,
                                    const std::vector<int> &horizonHours,
                                    std::chrono::system_clock::time_point now);
  ServiceResult<MlTask> mlTask(
      const std::string &taskNo,
      std::chrono::system_clock::time_point now =
          std::chrono::system_clock::now());
  // UC-M-04: training tasks time out after ten minutes and prediction tasks
  // after two; without this transition the running-task dedup would block the
  // task type forever because no ML subprocess exists yet.
  void completeTimedOutMlTasks(std::chrono::system_clock::time_point now);
  std::vector<PredictionView>
  predictions(std::optional<std::int64_t> stationId,
              std::optional<int> horizonHour, std::int64_t fromAt = 0);

  static bool validBucket(const std::string &bucket) {
    return bucket == "day" || bucket == "hour";
  }

  static constexpr std::int64_t kTrainTimeoutSeconds = 600;
  static constexpr std::int64_t kPredictTimeoutSeconds = 120;

private:
  void completeTimedOutMlTasksUnlocked(
      std::chrono::system_clock::time_point now);
  AdminRepository &repository_;
  ChargingRepository &charging_;
  ChargeFlowService &flows_;
  BusinessNumbers &numbers_;
  AnalyticsRepository *analytics_ = nullptr;
  MlTaskLauncher *launcher_ = nullptr;
  std::mutex mlStartMutex_;
};

} // namespace ncs::core::application
