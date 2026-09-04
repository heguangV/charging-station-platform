#include "core/application/admin_ops_service.h"

#include <map>

namespace ncs::core::application {
namespace {

constexpr std::int64_t kMaximumStatsRangeSeconds = 90LL * 24 * 3600;
constexpr std::int64_t kDefaultStatsRangeSeconds = 30LL * 24 * 3600;

bool validReason(const std::string_view reason) {
  if (reason.size() < 2 || reason.size() > 200)
    return false;
  for (const unsigned char character : reason) {
    if (character < 0x20 || character == 0x7f)
      return false;
  }
  return true;
}

bool isOverdueMlTask(const MlTask &task,
                     const std::chrono::system_clock::time_point now) {
  const auto nowSeconds =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();
  const std::int64_t timeout =
      task.taskType == "TRAIN" ? AdminOpsService::kTrainTimeoutSeconds
                               : AdminOpsService::kPredictTimeoutSeconds;
  return task.createdAt <= nowSeconds - timeout;
}

} // namespace

AdminFlowPage AdminOpsService::flows(const AdminFlowQuery &query) {
  return repository_.flows(query);
}

ServiceResult<FlowView> AdminOpsService::forceRelease(
    const std::int64_t actorAdminId, const std::string &flowNo,
    const std::string &reason, const int nextChargerStatus,
    const std::int64_t flowVersion,
    const std::chrono::system_clock::time_point now) {
  if (!validReason(reason))
    return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
  const auto result = flows_.adminForceRelease(flowNo, reason,
                                               nextChargerStatus, flowVersion,
                                               now);
  if (result.ok()) {
    repository_.addAuditEvent(AuditEvent{
        actorAdminId, "FORCE_RELEASE", "FLOW", flowNo, reason,
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
            .count()});
  }
  return result;
}

ServiceResult<RevenueStats> AdminOpsService::revenueStats(
    const std::int64_t fromAt, const std::int64_t toAt,
    const std::optional<std::int64_t> stationId, const std::string &bucket,
    const std::chrono::system_clock::time_point now) {
  if (!validBucket(bucket))
    return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
  const std::int64_t resolvedTo =
      toAt > 0
          ? toAt
          : std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
                .count();
  const std::int64_t resolvedFrom =
      fromAt > 0 ? fromAt : resolvedTo - kDefaultStatsRangeSeconds;
  if (fromAt < 0 || toAt < 0 || resolvedFrom < 0 ||
      resolvedFrom > resolvedTo ||
      resolvedTo - resolvedFrom > kMaximumStatsRangeSeconds)
    return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
  const std::int64_t bucketSeconds = bucket == "day" ? 86400 : 3600;
  const auto orders =
      repository_.settledOrders(resolvedFrom, resolvedTo, stationId);
  RevenueStats stats;
  std::map<std::int64_t, RevenueBucket> grouped;
  for (const auto &order : orders) {
    const std::int64_t bucketStart =
        *order.settledAt / bucketSeconds * bucketSeconds;
    RevenueBucket &entry = grouped[bucketStart];
    entry.bucketStart = bucketStart;
    entry.amountCent += order.amountCent;
    entry.energyMwh += order.energyMwh;
    entry.orderCount += 1;
    stats.totalAmountCent += order.amountCent;
    stats.totalEnergyMwh += order.energyMwh;
    stats.totalOrderCount += 1;
  }
  for (const auto &[start, entry] : grouped) {
    (void)start;
    stats.items.push_back(entry);
  }
  return {core::domain::ErrorCode::Ok, stats};
}

ChargerStatusStats AdminOpsService::chargerStatusStats(
    const std::optional<std::int64_t> stationId) {
  ChargerStatusStats stats;
  for (const auto &charger : charging_.chargers(stationId, std::nullopt,
                                                std::nullopt)) {
    switch (charger.status) {
    case ChargerStatus::Idle:
      ++stats.idleCount;
      break;
    case ChargerStatus::Occupied:
      ++stats.occupiedCount;
      break;
    case ChargerStatus::Faulty:
      ++stats.faultyCount;
      break;
    case ChargerStatus::Disabled:
      ++stats.disabledCount;
      break;
    case ChargerStatus::Restarting:
      ++stats.restartingCount;
      break;
    }
    if (charger.status == ChargerStatus::Idle ||
        charger.status == ChargerStatus::Occupied)
      ++stats.operationalCount;
    ++stats.totalCount;
  }
  stats.healthPercent =
      stats.totalCount == 0
          ? 0
          : static_cast<int>(stats.operationalCount * 100 / stats.totalCount);
  return stats;
}

std::vector<AuditEvent> AdminOpsService::auditEvents(
    const AuditEventQuery &query) {
  return repository_.auditEvents(query);
}

ServiceResult<BackupRecord> AdminOpsService::createBackup(
    const std::int64_t actorAdminId,
    const std::chrono::system_clock::time_point now) {
  const auto nowSeconds =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();
  BackupRecord record;
  record.backupNo = numbers_.next("BK", now);
  record.createdAt = nowSeconds;
  record.status = "PENDING";
  repository_.addBackup(record);
  if (!repository_.createBackupSnapshot(record)) {
    record.status = "FAILED";
    repository_.saveBackup(record);
    repository_.addAuditEvent(AuditEvent{actorAdminId, "BACKUP_FAILED",
                                         "BACKUP", record.backupNo, {},
                                         nowSeconds});
    return {core::domain::ErrorCode::TransactionFailed, std::nullopt};
  }
  record.status = "SUCCEEDED";
  repository_.saveBackup(record);
  repository_.addAuditEvent(AuditEvent{actorAdminId, "BACKUP_CREATED",
                                       "BACKUP", record.backupNo, {},
                                       nowSeconds});
  return {core::domain::ErrorCode::Ok, record};
}

std::vector<BackupRecord> AdminOpsService::backups() {
  return repository_.backups();
}

ServiceResult<BackupRecord> AdminOpsService::verifyBackup(
    const std::int64_t actorAdminId, const std::string &backupNo,
    const std::chrono::system_clock::time_point now) {
  const auto record = repository_.backup(backupNo);
  if (!record)
    return {core::domain::ErrorCode::NotFound, std::nullopt};
  BackupRecord verified = *record;
  verified.verificationStatus = "RUNNING";
  repository_.saveBackup(verified);
  const bool valid = repository_.verifyBackupSnapshot(verified);
  verified.verificationStatus = valid ? "SUCCEEDED" : "FAILED";
  verified.verifiedAt =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();
  repository_.saveBackup(verified);
  repository_.addAuditEvent(AuditEvent{actorAdminId,
                                       valid ? "BACKUP_VERIFIED"
                                             : "BACKUP_VERIFICATION_FAILED",
                                       "BACKUP", backupNo, {},
                                       *verified.verifiedAt});
  return valid ? ServiceResult<BackupRecord>{core::domain::ErrorCode::Ok,
                                             verified}
               : ServiceResult<BackupRecord>{
                     core::domain::ErrorCode::TransactionFailed, std::nullopt};
}

ServiceResult<MlTask> AdminOpsService::startMlTask(
    const std::int64_t actorAdminId, const std::string &taskType,
    const std::vector<int> &horizonHours,
    const std::chrono::system_clock::time_point now) {
  if (taskType != "TRAIN" && taskType != "PREDICT")
    return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
  if (taskType == "PREDICT") {
    if (horizonHours.empty())
      return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
    for (const int horizon : horizonHours) {
      if (horizon != 1 && horizon != 6 && horizon != 24)
        return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
    }
  }
  std::lock_guard startLock(mlStartMutex_);
  completeTimedOutMlTasksUnlocked(now);
  if (const auto running = repository_.runningMlTask(taskType))
    return {core::domain::ErrorCode::Ok, *running};
  const auto nowSeconds =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();
  MlTask task;
  task.taskNo = numbers_.next("ML", now);
  task.taskType = taskType;
  task.status = "RUNNING";
  task.horizonHours = horizonHours;
  task.createdAt = nowSeconds;
  repository_.withTransaction([&] {
    repository_.addMlTask(task);
    repository_.addAuditEvent(AuditEvent{actorAdminId, "ML_TASK_STARTED",
                                         "ML_TASK", task.taskNo, taskType,
                                         nowSeconds});
  });
  bool launched = true;
  if (launcher_) {
    try {
      launched = launcher_->start(task);
    } catch (...) {
      launched = false;
    }
  }
  if (!launched) {
    task.status = "FAILED";
    task.finishedAt = nowSeconds;
    task.errorSummary = "ML 子进程启动失败";
    if (repository_.tryFinishMlTask(task, false) && analytics_)
      analytics_->markPredictionsStale();
    return {core::domain::ErrorCode::ExternalServiceUnavailable, std::nullopt};
  }
  return {core::domain::ErrorCode::Ok, task};
}

ServiceResult<MlTask> AdminOpsService::mlTask(
    const std::string &taskNo,
    const std::chrono::system_clock::time_point now) {
  const auto task = repository_.mlTask(taskNo);
  if (!task)
    return {core::domain::ErrorCode::NotFound, std::nullopt};
  if ((task->status == "PENDING" || task->status == "RUNNING") &&
      isOverdueMlTask(*task, now)) {
    completeTimedOutMlTasks(now);
    const auto completed = repository_.mlTask(taskNo);
    if (!completed)
      return {core::domain::ErrorCode::NotFound, std::nullopt};
    return {core::domain::ErrorCode::Ok, *completed};
  }
  return {core::domain::ErrorCode::Ok, *task};
}

void AdminOpsService::completeTimedOutMlTasks(
    const std::chrono::system_clock::time_point now) {
  std::lock_guard lock(mlStartMutex_);
  completeTimedOutMlTasksUnlocked(now);
}

void AdminOpsService::completeTimedOutMlTasksUnlocked(
    const std::chrono::system_clock::time_point now) {
  const auto nowSeconds =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();
  for (auto task : repository_.overdueMlTasks(
           nowSeconds - kTrainTimeoutSeconds,
           nowSeconds - kPredictTimeoutSeconds)) {
    task.status = "TIMED_OUT";
    task.finishedAt = nowSeconds;
    task.errorSummary = "任务超时未完成";
    if (repository_.tryFinishMlTask(task, false)) {
      if (launcher_)
        launcher_->stop(task.taskNo);
      if (analytics_)
        analytics_->markPredictionsStale();
    }
  }
}

std::vector<PredictionView> AdminOpsService::predictions(
    const std::optional<std::int64_t> stationId,
    const std::optional<int> horizonHour, const std::int64_t fromAt) {
  if (!analytics_)
    return {};
  std::vector<PredictionView> result;
  for (const auto &value :
       analytics_->predictions(stationId, horizonHour, fromAt)) {
    result.push_back(PredictionView{
        value.stationId, value.horizonHour, value.modelVersionNo,
        value.generatedAt, value.targetAt, value.predictedEnergyMwh,
        value.predictedFreeCount, value.isPeak, value.stale});
  }
  return result;
}

} // namespace ncs::core::application
