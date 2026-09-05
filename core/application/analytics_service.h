#pragma once

#include "core/application/admin_repository.h"
#include "core/application/business_numbers.h"
#include "core/application/charging_repository.h"
#include "core/application/service_result.h"
#include "core/application/session_manager.h"
#include "core/application/user_account_repository.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ncs::core::application {

struct HourlyMetric {
  std::int64_t stationId = 0;
  std::string stationName;
  std::int64_t bucketAt = 0;
  std::int64_t energyMwh = 0;
  int orderCount = 0;
  int fastOrderCount = 0;
  int slowOrderCount = 0;
  int operationalChargerCount = 0;
  std::int64_t busyDeviceSeconds = 0;
};

struct HourlyMetricPage {
  std::vector<HourlyMetric> items;
  std::string nextCursor;
};

struct ModelVersion {
  std::string versionNo;
  std::string taskNo;
  std::string algorithm;
  int featureSchemaVersion = 1;
  std::int64_t randomSeed = 20260901;
  std::int64_t trainFromAt = 0;
  std::int64_t trainToAt = 0;
  double mae = 0;
  double rmse = 0;
  double mape = 0;
  double wape = 0;
  double baselineMae = 0;
  double baselineRmse = 0;
  int excludedSampleCount = 0;
  bool qualified = false;
  std::string artifactChecksum;
  std::string artifactPath;
  std::int64_t createdAt = 0;
};

struct LoadPrediction {
  std::int64_t stationId = 0;
  int horizonHour = 1;
  std::string modelVersionNo;
  std::int64_t generatedAt = 0;
  std::int64_t targetAt = 0;
  std::int64_t predictedEnergyMwh = 0;
  int predictedFreeCount = 0;
  bool isPeak = false;
  bool stale = false;
};

struct RevenuePoint {
  std::int64_t bucketAt = 0;
  std::int64_t revenueCent = 0;
  std::int64_t energyMwh = 0;
  int orderCount = 0;
};

struct StationRank {
  std::int64_t stationId = 0;
  std::string stationName;
  std::int64_t energyMwh = 0;
  std::int64_t revenueCent = 0;
  int orderCount = 0;
};

struct HeatmapPoint {
  int weekday = 0; // 1 = Monday, 7 = Sunday (UTC buckets).
  int hour = 0;
  std::int64_t energyMwh = 0;
  int orderCount = 0;
};

struct DashboardSnapshot {
  int schemaVersion = 1;
  std::int64_t dataVersion = 0;
  std::int64_t generatedAt = 0;
  bool stale = false;
  std::int64_t totalRevenueCent = 0;
  int totalChargeCount = 0;
  int registeredUserCount = 0;
  int stationCount = 0;
  int idleCount = 0;
  int inUseCount = 0;
  int faultCount = 0;
  int restartingCount = 0;
  int disabledCount = 0;
  int fastChargeCount = 0;
  int slowChargeCount = 0;
  std::vector<RevenuePoint> revenue30d;
  std::vector<StationRank> stationRanking;
  std::vector<HeatmapPoint> hourlyHeatmap;
  std::vector<LoadPrediction> prediction24h;
};

class AnalyticsRepository {
public:
  virtual ~AnalyticsRepository() = default;
  virtual void refreshHourlyMetrics(std::int64_t fromAt,
                                    std::int64_t toAt) = 0;
  virtual HourlyMetricPage hourlyMetrics(
      std::int64_t fromAt, std::int64_t toAt,
      std::optional<std::int64_t> stationId, std::string_view cursor,
      int limit) = 0;
  virtual std::int64_t nextDashboardVersion() = 0;
  virtual void addModelVersion(const ModelVersion &version) = 0;
  virtual std::optional<ModelVersion>
  modelVersion(std::string_view versionNo) = 0;
  virtual std::optional<ModelVersion> latestQualifiedModel() = 0;
  virtual void upsertPredictions(const std::vector<LoadPrediction> &items) = 0;
  virtual std::vector<LoadPrediction> predictions(
      std::optional<std::int64_t> stationId,
      std::optional<int> horizonHour, std::int64_t fromAt) = 0;
  virtual void markPredictionsStale() = 0;
  virtual void cleanupAnalytics(std::int64_t now) = 0;
};

class ModelArtifactStore {
public:
  virtual ~ModelArtifactStore() = default;
  // Verifies the task-owned staging artifact. Qualified artifacts atomically
  // replace the active model; unqualified artifacts are discarded.
  virtual bool verify(std::string_view taskNo,
                      std::string_view checksum) const = 0;
  virtual bool verifyArtifact(std::string_view path,
                              std::string_view checksum) const = 0;
  virtual bool finalize(std::string_view taskNo, bool qualified,
                        std::string_view modelVersionNo) = 0;
  virtual std::string activePath() const = 0;
  virtual std::string artifactPath(std::string_view modelVersionNo) const = 0;
};

class DashboardService final {
public:
  DashboardService(AnalyticsRepository &analytics, AdminRepository &admin,
                   ChargingRepository &charging,
                   UserAccountRepository &accounts)
      : analytics_(analytics), admin_(admin), charging_(charging),
        accounts_(accounts) {}

  ServiceResult<DashboardSnapshot>
  refresh(std::chrono::system_clock::time_point now);
  std::optional<DashboardSnapshot> current() const;
  void markStale();

private:
  AnalyticsRepository &analytics_;
  AdminRepository &admin_;
  ChargingRepository &charging_;
  UserAccountRepository &accounts_;
  mutable std::mutex mutex_;
  std::optional<DashboardSnapshot> current_;
};

enum class MlCapability { ReadFeatures, RegisterModel, WritePredictions, Complete };

class MlService final {
public:
  MlService(AnalyticsRepository &analytics, AdminRepository &admin,
            ChargingRepository &charging, BusinessNumbers &numbers,
            ModelArtifactStore *artifacts = nullptr)
      : analytics_(analytics), admin_(admin), charging_(charging),
        numbers_(numbers), artifacts_(artifacts) {}

  bool authorize(const AuthContext &auth, std::string_view taskNo,
                 MlCapability capability) const;
  ServiceResult<HourlyMetricPage> features(
      std::string_view taskNo, std::int64_t fromAt, std::int64_t toAt,
      std::optional<std::int64_t> stationId, std::string_view cursor,
      int limit, std::chrono::system_clock::time_point now);
  ServiceResult<ModelVersion>
  registerModel(std::string_view taskNo, ModelVersion version,
                std::chrono::system_clock::time_point now);
  ServiceResult<std::size_t>
  writePredictions(std::string_view taskNo,
                   const std::vector<LoadPrediction> &items,
                   std::chrono::system_clock::time_point now);
  ServiceResult<MlTask>
  complete(std::string_view taskNo, bool succeeded,
           std::string modelVersionNo, std::string metricsSummary,
           std::string errorSummary,
           std::chrono::system_clock::time_point now);
  std::vector<LoadPrediction> predictions(
      std::optional<std::int64_t> stationId,
      std::optional<int> horizonHour, std::int64_t fromAt);

private:
  static bool safeSummary(std::string_view value);
  AnalyticsRepository &analytics_;
  AdminRepository &admin_;
  ChargingRepository &charging_;
  BusinessNumbers &numbers_;
  ModelArtifactStore *artifacts_ = nullptr;
};

} // namespace ncs::core::application
