#include "core/application/analytics_service.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

namespace ncs::core::application {
namespace {

constexpr std::int64_t kHour = 3600;
constexpr std::int64_t kDay = 24 * kHour;
constexpr std::int64_t kMlWindow = 90 * kDay;
constexpr std::int64_t kTrainTimeout = 10 * 60;
constexpr std::int64_t kPredictTimeout = 2 * 60;

std::int64_t unixSeconds(const std::chrono::system_clock::time_point now) {
  return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
      .count();
}

bool running(const MlTask &task) {
  return task.status == "PENDING" || task.status == "RUNNING";
}

bool validMetricCursor(const std::string_view cursor) {
  if (cursor.empty())
    return true;
  const auto separator = cursor.find(':');
  if (separator == std::string_view::npos ||
      cursor.find(':', separator + 1) != std::string_view::npos)
    return false;
  std::int64_t bucket = 0;
  std::int64_t station = 0;
  const auto left = cursor.substr(0, separator);
  const auto right = cursor.substr(separator + 1);
  const auto first = std::from_chars(left.data(), left.data() + left.size(), bucket);
  const auto second = std::from_chars(right.data(), right.data() + right.size(), station);
  return !left.empty() && !right.empty() && first.ec == std::errc{} &&
         first.ptr == left.data() + left.size() && second.ec == std::errc{} &&
         second.ptr == right.data() + right.size() && bucket >= 0 && station > 0;
}

} // namespace

ServiceResult<DashboardSnapshot>
DashboardService::refresh(const std::chrono::system_clock::time_point now) {
  try {
    const std::int64_t generatedAt = unixSeconds(now);
    const std::int64_t fromAt = generatedAt / kDay * kDay - 29 * kDay;
    DashboardSnapshot snapshot;
    // Dashboard aggregation is a coherent read snapshot and must not reserve
    // SQLite's writer lock for the duration of the 30-day scan.
    admin_.withReadTransaction([&] {
      snapshot.generatedAt = generatedAt;
      const auto accounts = accounts_.listAccounts();
      snapshot.registeredUserCount = static_cast<int>(std::count_if(
          accounts.begin(), accounts.end(), [](const auto &account) {
            return !account.deleted;
          }));
      const auto stations = charging_.stations();
      snapshot.stationCount = static_cast<int>(stations.size());
      for (const auto &charger :
           charging_.chargers(std::nullopt, std::nullopt, std::nullopt)) {
        switch (charger.status) {
        case ChargerStatus::Idle: ++snapshot.idleCount; break;
        case ChargerStatus::Occupied: ++snapshot.inUseCount; break;
        case ChargerStatus::Faulty: ++snapshot.faultCount; break;
        case ChargerStatus::Restarting: ++snapshot.restartingCount; break;
        case ChargerStatus::Disabled: ++snapshot.disabledCount; break;
        }
      }

      std::map<std::int64_t, RevenuePoint> daily;
      std::map<std::int64_t, StationRank> ranking;
      std::map<std::pair<int, int>, HeatmapPoint> heatmap;
      for (const auto &order :
           admin_.settledOrders(fromAt, generatedAt, std::nullopt)) {
        if (!order.settledAt)
          continue;
        snapshot.totalRevenueCent += order.amountCent;
        ++snapshot.totalChargeCount;
        if (order.chargerType == ChargerType::DcFast)
          ++snapshot.fastChargeCount;
        else
          ++snapshot.slowChargeCount;
        auto &day = daily[*order.settledAt / kDay * kDay];
        day.bucketAt = *order.settledAt / kDay * kDay;
        day.revenueCent += order.amountCent;
        day.energyMwh += order.energyMwh;
        ++day.orderCount;
        auto &station = ranking[order.stationId];
        station.stationId = order.stationId;
        station.stationName = order.stationName;
        station.energyMwh += order.energyMwh;
        station.revenueCent += order.amountCent;
        ++station.orderCount;

        const std::int64_t activityAt =
            order.startedAt.value_or(*order.settledAt);
        const std::int64_t days = activityAt / kDay;
        const int weekday = static_cast<int>((days + 3) % 7 + 1);
        const int hour = static_cast<int>((activityAt % kDay) / kHour);
        auto &heat = heatmap[{weekday, hour}];
        heat.weekday = weekday;
        heat.hour = hour;
        heat.energyMwh += order.energyMwh;
        ++heat.orderCount;
      }
      for (std::int64_t day = fromAt / kDay * kDay;
           day <= generatedAt / kDay * kDay; day += kDay) {
        RevenuePoint value = daily[day];
        value.bucketAt = day;
        snapshot.revenue30d.push_back(value);
      }
      for (auto &[id, value] : ranking) {
        (void)id;
        snapshot.stationRanking.push_back(std::move(value));
      }
      std::sort(snapshot.stationRanking.begin(), snapshot.stationRanking.end(),
                [](const auto &left, const auto &right) {
                  return left.energyMwh > right.energyMwh ||
                         (left.energyMwh == right.energyMwh &&
                          left.stationId < right.stationId);
                });
      for (int weekday = 1; weekday <= 7; ++weekday) {
        for (int hour = 0; hour < 24; ++hour) {
          auto value = heatmap[{weekday, hour}];
          value.weekday = weekday;
          value.hour = hour;
          snapshot.hourlyHeatmap.push_back(value);
        }
      }
      snapshot.prediction24h =
          analytics_.predictions(std::nullopt, 24, generatedAt);
    });
    snapshot.dataVersion = analytics_.nextDashboardVersion();
    {
      std::lock_guard lock(mutex_);
      current_ = snapshot;
    }
    return {core::domain::ErrorCode::Ok, std::move(snapshot)};
  } catch (...) {
    markStale();
    return {core::domain::ErrorCode::TransactionFailed, std::nullopt};
  }
}

std::optional<DashboardSnapshot> DashboardService::current() const {
  std::lock_guard lock(mutex_);
  return current_;
}

void DashboardService::markStale() {
  std::lock_guard lock(mutex_);
  if (current_)
    current_->stale = true;
}

bool MlService::authorize(const AuthContext &auth, const std::string_view taskNo,
                          const MlCapability capability) const {
  if (auth.tokenKind != TokenKind::MlTask ||
      !SessionManager::hasRole(auth, Role::MlWorker) ||
      auth.principalId != "ml:" + std::string(taskNo))
    return false;
  if (capability == MlCapability::RegisterModel)
    return SessionManager::hasRole(auth, Role::MlTrainer);
  if (capability == MlCapability::WritePredictions)
    return SessionManager::hasRole(auth, Role::MlPredictor);
  return true;
}

ServiceResult<HourlyMetricPage> MlService::features(
    const std::string_view taskNo, const std::int64_t fromAt,
    const std::int64_t toAt, const std::optional<std::int64_t> stationId,
    const std::string_view cursor, const int limit,
    const std::chrono::system_clock::time_point now) {
  const auto task = admin_.mlTask(std::string(taskNo));
  const std::int64_t nowAt = unixSeconds(now);
  if (!task || !running(*task) || fromAt < 0 || toAt <= fromAt ||
      toAt > nowAt + kDay || toAt - fromAt > kMlWindow || limit < 1 ||
      limit > 5000 || !validMetricCursor(cursor) ||
      (stationId && !charging_.station(*stationId))) {
    return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
  }
  const std::int64_t effectiveToAt =
      std::min(toAt / kHour * kHour, task->createdAt / kHour * kHour);
  if (effectiveToAt <= fromAt)
    return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
  // Rebuild once at the beginning of a cursor walk. Rebuilding every page is
  // wasteful and would make a multi-page export observe different snapshots.
  if (cursor.empty())
    analytics_.refreshHourlyMetrics(fromAt, effectiveToAt);
  return {core::domain::ErrorCode::Ok,
          analytics_.hourlyMetrics(fromAt, effectiveToAt, stationId, cursor,
                                   limit)};
}

ServiceResult<ModelVersion> MlService::registerModel(
    const std::string_view taskNo, ModelVersion version,
    const std::chrono::system_clock::time_point now) {
  const auto task = admin_.mlTask(std::string(taskNo));
  const std::int64_t nowAt = unixSeconds(now);
  if (!task || !running(*task) || task->taskType != "TRAIN" ||
      version.algorithm != "RandomForestRegressor" ||
      version.featureSchemaVersion != 1 || version.randomSeed != 20260901 ||
      version.trainFromAt < 0 || version.trainToAt <= version.trainFromAt ||
      version.trainToAt > nowAt || version.trainToAt - version.trainFromAt > kMlWindow ||
      !std::isfinite(version.mae) || !std::isfinite(version.rmse) ||
      !std::isfinite(version.mape) || !std::isfinite(version.wape) ||
      !std::isfinite(version.baselineMae) ||
      !std::isfinite(version.baselineRmse) || version.mae < 0 ||
      version.rmse < 0 || version.mape < 0 || version.wape < 0 ||
      version.baselineMae < 0 || version.baselineRmse < 0 ||
      version.excludedSampleCount < 0 ||
      version.artifactChecksum.size() != 64 ||
      std::any_of(version.artifactChecksum.begin(), version.artifactChecksum.end(),
                  [](const unsigned char value) { return !std::isxdigit(value); })) {
    return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
  }
  version.versionNo = numbers_.next("MODEL", now);
  version.taskNo = std::string(taskNo);
  std::transform(version.artifactChecksum.begin(),
                 version.artifactChecksum.end(),
                 version.artifactChecksum.begin(), [](const unsigned char value) {
                   return static_cast<char>(std::tolower(value));
                 });
  version.qualified = version.mae < version.baselineMae &&
                      version.rmse < version.baselineRmse;
  version.createdAt = nowAt;
  if (artifacts_) {
    if (!artifacts_->verify(taskNo, version.artifactChecksum))
      return {core::domain::ErrorCode::ExternalServiceUnavailable, std::nullopt};
    version.artifactPath = version.qualified
                               ? artifacts_->artifactPath(version.versionNo)
                               : std::string();
  } else {
    version.artifactPath = "ml/models/load_rf.pkl";
  }
  analytics_.addModelVersion(version);
  return {core::domain::ErrorCode::Ok, std::move(version)};
}

ServiceResult<std::size_t> MlService::writePredictions(
    const std::string_view taskNo, const std::vector<LoadPrediction> &items,
    const std::chrono::system_clock::time_point now) {
  const auto task = admin_.mlTask(std::string(taskNo));
  const std::int64_t nowAt = unixSeconds(now);
  if (!task || !running(*task) || task->taskType != "PREDICT" ||
      items.empty() || items.size() > 5000) {
    return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
  }
  const std::string &modelVersionNo = items.front().modelVersionNo;
  if (modelVersionNo.empty() ||
      std::any_of(items.begin(), items.end(), [&](const auto &item) {
        return item.modelVersionNo != modelVersionNo;
      }))
    return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
  if (modelVersionNo != "BASELINE") {
    const auto model = analytics_.modelVersion(modelVersionNo);
    const auto modelTask = model ? admin_.mlTask(model->taskNo) : std::nullopt;
    if (!model || !model->qualified || !modelTask ||
        modelTask->status != "SUCCEEDED")
      return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
  }
  std::set<std::int64_t> stationIds;
  for (const auto &station : charging_.stations())
    stationIds.insert(station.id);
  std::map<std::int64_t, int> operationalCounts;
  for (const auto &charger :
       charging_.chargers(std::nullopt, std::nullopt, std::nullopt)) {
    if (charger.status == ChargerStatus::Idle ||
        charger.status == ChargerStatus::Occupied)
      ++operationalCounts[charger.stationId];
  }
  std::set<std::tuple<std::int64_t, std::string, std::int64_t>> uniqueKeys;
  for (const auto &item : items) {
    if (!stationIds.count(item.stationId) ||
        (item.horizonHour != 1 && item.horizonHour != 6 &&
         item.horizonHour != 24) ||
        std::find(task->horizonHours.begin(), task->horizonHours.end(),
                  item.horizonHour) == task->horizonHours.end() ||
        item.generatedAt < nowAt - kDay || item.generatedAt > nowAt + 300 ||
        item.targetAt != item.generatedAt + item.horizonHour * kHour ||
        item.predictedEnergyMwh < 0 || item.predictedEnergyMwh > 1000000000000LL ||
        item.predictedFreeCount < 0 ||
        item.predictedFreeCount > operationalCounts[item.stationId] ||
        !uniqueKeys.emplace(item.stationId, item.modelVersionNo,
                            item.targetAt).second) {
      return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
    }
  }
  analytics_.upsertPredictions(items);
  return {core::domain::ErrorCode::Ok, items.size()};
}

ServiceResult<MlTask> MlService::complete(
    const std::string_view taskNo, const bool succeeded,
    std::string modelVersionNo, std::string metricsSummary,
    std::string errorSummary, const std::chrono::system_clock::time_point now) {
  if (!safeSummary(metricsSummary) || !safeSummary(errorSummary) ||
      (succeeded && !errorSummary.empty()) || (!succeeded && errorSummary.empty()))
    return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
  // Artifact activation performs file I/O and must not extend the SQLite
  // write-lock window. It runs before the finishing transaction; the
  // transaction re-validates the task so a concurrent timeout cannot
  // activate an artifact for an already-terminated task.
  const auto preliminary = admin_.mlTask(std::string(taskNo));
  if (!preliminary)
    return {core::domain::ErrorCode::NotFound, std::nullopt};
  if (preliminary->taskType == "TRAIN" && artifacts_) {
    if (succeeded) {
      const auto version = analytics_.modelVersion(modelVersionNo);
      if (!version || version->taskNo != taskNo)
        return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
      if (!artifacts_->finalize(taskNo, version->qualified, modelVersionNo))
        return {core::domain::ErrorCode::ExternalServiceUnavailable,
                std::nullopt};
    } else {
      (void)artifacts_->finalize(taskNo, false, {});
    }
  }
  core::domain::ErrorCode error = core::domain::ErrorCode::Ok;
  std::optional<MlTask> completed;
  const std::int64_t receivedAt = unixSeconds(now);
  admin_.withTransaction([&] {
    auto task = admin_.mlTask(std::string(taskNo));
    if (!task) {
      error = core::domain::ErrorCode::NotFound;
      return;
    }
    const std::int64_t timeout =
        task->taskType == "TRAIN" ? kTrainTimeout : kPredictTimeout;
    const bool receivedBeforeDeadline = receivedAt <= task->createdAt + timeout;
    const bool recoveringTimeout =
        task->status == "TIMED_OUT" && receivedBeforeDeadline;
    if ((!running(*task) && !recoveringTimeout) || !receivedBeforeDeadline) {
      error = core::domain::ErrorCode::InvalidStateTransition;
      return;
    }
    if (succeeded && task->taskType == "TRAIN") {
      const auto version = analytics_.modelVersion(modelVersionNo);
      if (!version || version->taskNo != taskNo) {
        error = core::domain::ErrorCode::ValidationFailed;
        return;
      }
    }
    task->status = succeeded ? "SUCCEEDED" : "FAILED";
    task->modelVersion = modelVersionNo;
    task->metricsSummary = metricsSummary;
    task->errorSummary = errorSummary;
    task->finishedAt = receivedAt;
    if (!admin_.tryFinishMlTask(*task, recoveringTimeout)) {
      error = core::domain::ErrorCode::InvalidStateTransition;
      return;
    }
    completed = std::move(task);
  });
  if (error != core::domain::ErrorCode::Ok)
    return {error, std::nullopt};
  if (!succeeded)
    analytics_.markPredictionsStale();
  return {core::domain::ErrorCode::Ok, std::move(*completed)};
}

std::vector<LoadPrediction> MlService::predictions(
    const std::optional<std::int64_t> stationId,
    const std::optional<int> horizonHour, const std::int64_t fromAt) {
  return analytics_.predictions(stationId, horizonHour, fromAt);
}

bool MlService::safeSummary(const std::string_view value) {
  if (value.size() > 500)
    return false;
  return std::none_of(value.begin(), value.end(), [](const unsigned char c) {
    return c < 0x20 && c != '\t';
  });
}

} // namespace ncs::core::application
