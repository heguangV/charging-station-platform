#pragma once

#include "core/application/business_numbers.h"
#include "core/application/analytics_service.h"
#include "core/application/admin_repository.h"
#include "core/application/charging_repository.h"
#include "core/application/idempotency_service.h"
#include "core/application/readiness_probe.h"
#include "core/application/user_account_repository.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ncs::infrastructure::sqlite {

// Persistent adapter for the complete user charging aggregate. Connections
// are opened by the calling worker and never shared across threads. Nested
// repository calls reuse the connection owned by withTransaction().
class SqliteRepository final
    : public core::application::UserAccountRepository,
      public core::application::WalletMirror,
      public core::application::ChargingRepository,
      public core::application::ReadinessProbe,
      public core::application::BusinessNumberSequenceStore,
      public core::application::IdempotencyPersistence,
      public core::application::AdminRepository,
      public core::application::AnalyticsRepository {
public:
  explicit SqliteRepository(std::string databasePath);
  ~SqliteRepository() override;

  SqliteRepository(const SqliteRepository &) = delete;
  SqliteRepository &operator=(const SqliteRepository &) = delete;

  std::optional<core::application::UserAccount>
  findById(std::int64_t id) const override;
  std::optional<core::application::UserAccount>
  findByPhone(std::string_view phone) const override;
  std::optional<core::application::UserAccount>
  findByLoginName(std::string_view loginName) const override;
  core::application::AccountWriteResult
  create(core::application::UserAccount &account) override;
  core::application::AccountWriteResult
  updateNickname(std::int64_t id, std::int64_t expectedVersion,
                 std::string nickname,
                 core::application::UserAccount &updated) override;
  core::application::AccountWriteResult
  updateStatus(std::int64_t id, int status,
               core::application::UserAccount &updated) override;
  core::application::AccountWriteResult
  updateCredential(std::int64_t id, std::string username,
                   std::string passwordHash,
                   core::application::UserAccount &updated) override;
  core::application::AccountWriteResult
  replacePasswordHash(std::int64_t id, std::string_view expectedCurrentHash,
                      std::string_view newPasswordHash) override;
  core::application::AccountWriteResult
  updateAvatar(std::int64_t id, core::application::AvatarData avatar,
               core::application::UserAccount &updated) override;
  core::application::AccountWriteResult
  anonymize(std::int64_t id, core::application::UserAccount &updated) override;
  std::vector<core::application::UserAccount> listAccounts() override;
  void applyWalletState(std::int64_t userId, std::int64_t balanceCent,
                        std::int64_t debtCent) override;
  void setActiveFlowFlag(std::int64_t userId, bool hasActiveFlow) override;

  void withTransaction(const std::function<void()> &work) override;
  void withReadTransaction(const std::function<void()> &work) override;
  core::application::WalletAccount wallet(std::int64_t userId) override;
  void saveWallet(const core::application::WalletAccount &wallet) override;
  void addWalletTransaction(
      const core::application::WalletTransaction &transaction) override;
  std::vector<core::application::WalletTransaction> walletTransactions(
      std::int64_t userId,
      std::optional<core::application::WalletTransactionType> type,
      std::int64_t fromAt, std::int64_t toAt) override;
  void addRechargeOrder(const core::application::RechargeOrder &order) override;

  std::vector<core::application::Station> stations() override;
  std::optional<core::application::Station>
  station(std::int64_t stationId) override;
  std::vector<core::application::Charger>
  chargers(std::optional<std::int64_t> stationId,
           std::optional<core::application::ChargerType> type,
           std::optional<core::application::ChargerStatus> status) override;
  std::optional<core::application::Charger>
  charger(std::int64_t chargerId) override;
  void saveCharger(const core::application::Charger &charger) override;
  std::optional<core::application::RegionTariff>
  effectiveTariff(const std::string &adcode, std::int64_t at) override;

  void addFlow(const core::application::ChargingFlow &flow) override;
  void saveFlow(const core::application::ChargingFlow &flow) override;
  std::optional<core::application::ChargingFlow>
  flow(const std::string &flowNo) override;
  std::optional<core::application::ChargingFlow>
  activeFlow(std::int64_t userId) override;
  std::vector<core::application::ChargingFlow>
  flowsWithStatus(int status) override;
  void addFlowEvent(const core::application::FlowEvent &event) override;
  void addChargerStatusEvent(
      const core::application::ChargerStatusEvent &event) override;
  std::vector<core::application::OutboxEvent>
  pollOutbox(std::int64_t now, int limit) override;
  void markOutboxDelivered(const std::vector<std::int64_t> &ids) override;
  void markOutboxAttempted(const std::vector<std::int64_t> &ids) override;
  void markOutboxDead(const std::vector<std::int64_t> &ids) override;
  void enqueue(std::int64_t stationId, core::application::ChargerType type,
               const std::string &flowNo) override;
  void dequeue(std::int64_t stationId, core::application::ChargerType type,
               const std::string &flowNo) override;
  std::deque<std::string> queue(std::int64_t stationId,
                                core::application::ChargerType type) override;

  void addOrder(const core::application::ChargingOrder &order) override;
  void saveOrder(const core::application::ChargingOrder &order) override;
  std::optional<core::application::ChargingOrder>
  order(const std::string &orderNo) override;
  std::optional<core::application::ChargingOrder>
  orderByFlow(const std::string &flowNo) override;
  std::vector<core::application::ChargingOrder>
  orders(std::int64_t userId, std::optional<int> status, std::int64_t fromAt,
         std::int64_t toAt) override;
  bool addStation(core::application::Station &station) override;
  bool saveStation(const core::application::Station &station) override;
  bool stationCodeExists(const std::string &code) override;
  bool addCharger(core::application::Charger &charger) override;
  bool chargerCodeExists(const std::string &code) override;
  void addTariff(const core::application::RegionTariff &tariff) override;
  std::vector<core::application::RegionTariff>
  tariffVersions(std::optional<std::string> adcode) override;
  std::vector<core::application::ChargingFlow> allFlows() override;
  std::vector<core::application::ChargingOrder> allOrders() override;

  core::application::ReadinessStatus check() override;
  void refreshReadiness();
  std::int64_t nextBusinessSequence(std::string_view prefix,
                                    std::int64_t utcDay) override;
  std::optional<core::application::PersistedIdempotencyRecord>
  loadIdempotencyRecord(std::string_view scope, std::string_view key) override;
  void saveIdempotencyRecord(
      const core::application::PersistedIdempotencyRecord &record) override;
  void removeIdempotencyRecord(std::string_view scope,
                               std::string_view key) override;
  void cleanupIdempotencyRecords(std::int64_t now) override;
  std::size_t idempotencyRecordCount() override;

  void ensureDevelopmentAdmin(bool enabled);
  std::optional<core::application::AdminAccount>
  findAdminByUsername(std::string_view username) override;
  std::optional<core::application::AdminAccount>
  findAdminById(std::int64_t id) override;
  core::application::AdminUserPage
  listManagedUsers(const core::application::AdminUserQuery &query) override;
  std::optional<core::application::UserAccount>
  findManagedUser(std::int64_t id) override;
  core::application::AccountWriteResult updateManagedUserStatus(
      std::int64_t actorAdminId, std::int64_t userId, int status,
      std::string_view reason, std::int64_t expectedVersion, std::int64_t at,
      core::application::UserAccount &updated) override;
  void addAuditEvent(const core::application::AuditEvent &event) override;
  std::vector<core::application::AuditEvent>
  auditEvents(const core::application::AuditEventQuery &query) override;
  bool createStationWithChargers(
      core::application::Station &station,
      const core::application::InitialChargerSpec &spec) override;
  std::vector<core::application::Station>
  stations(std::optional<int> status, std::optional<std::string> adcode,
           const std::string &keyword) override;
  bool addChargers(
      std::vector<core::application::Charger> &chargers) override;
  void addTariffVersion(
      const core::application::RegionTariff &tariff) override;
  bool tariffOverlaps(const std::string &adcode, std::int64_t from,
                      std::int64_t to) override;
  std::int64_t addPriceAdjustment(
      const core::application::PriceAdjustment &adjustment) override;
  std::optional<core::application::PriceAdjustment>
  effectivePriceAdjustment(std::int64_t stationId, int chargerType,
                           std::int64_t at) override;
  void addDeviceCommand(
      const core::application::DeviceCommand &command) override;
  std::optional<core::application::DeviceCommand>
  deviceCommand(const std::string &commandNo) override;
  void saveDeviceCommand(
      const core::application::DeviceCommand &command) override;
  std::vector<core::application::DeviceCommand>
  dueDeviceCommands(std::int64_t now) override;
  std::optional<core::application::ChargingFlow>
  activeFlowOnCharger(std::int64_t chargerId) override;
  bool stationHasActiveFlow(std::int64_t stationId) override;
  core::application::AdminFlowPage
  flows(const core::application::AdminFlowQuery &query) override;
  std::vector<core::application::ChargingOrder>
  settledOrders(std::int64_t fromAt, std::int64_t toAt,
                std::optional<std::int64_t> stationId) override;
  std::optional<core::application::MlTask>
  runningMlTask(const std::string &taskType) override;
  std::vector<core::application::MlTask>
  overdueMlTasks(std::int64_t trainDeadline,
                 std::int64_t predictDeadline) override;
  void addMlTask(const core::application::MlTask &task) override;
  std::optional<core::application::MlTask>
  mlTask(const std::string &taskNo) override;
  void saveMlTask(const core::application::MlTask &task) override;
  bool tryFinishMlTask(const core::application::MlTask &task,
                       bool allowTimedOut) override;
  void addBackup(const core::application::BackupRecord &record) override;
  std::vector<core::application::BackupRecord> backups() override;
  std::optional<core::application::BackupRecord>
  backup(const std::string &backupNo) override;
  void saveBackup(const core::application::BackupRecord &record) override;
  bool createBackupSnapshot(
      core::application::BackupRecord &record) override;
  bool verifyBackupSnapshot(
      const core::application::BackupRecord &record) override;
  void cleanupAdminRecords(std::int64_t now) override;
  void refreshHourlyMetrics(std::int64_t fromAt,
                            std::int64_t toAt) override;
  core::application::HourlyMetricPage hourlyMetrics(
      std::int64_t fromAt, std::int64_t toAt,
      std::optional<std::int64_t> stationId, std::string_view cursor,
      int limit) override;
  std::int64_t nextDashboardVersion() override;
  void addModelVersion(
      const core::application::ModelVersion &version) override;
  std::optional<core::application::ModelVersion>
  modelVersion(std::string_view versionNo) override;
  std::optional<core::application::ModelVersion>
  latestQualifiedModel() override;
  void upsertPredictions(
      const std::vector<core::application::LoadPrediction> &items) override;
  std::vector<core::application::LoadPrediction> predictions(
      std::optional<std::int64_t> stationId,
      std::optional<int> horizonHour, std::int64_t fromAt) override;
  void markPredictionsStale() override;
  void cleanupAnalytics(std::int64_t now) override;
  const std::string &databasePath() const { return databasePath_; }

private:
  void initialize();
  core::application::ReadinessStatus probeDatabase();
  void pruneBackups();

  std::string databasePath_;
  std::mutex readinessMutex_;
  core::application::ReadinessStatus readiness_;
};

} // namespace ncs::infrastructure::sqlite
