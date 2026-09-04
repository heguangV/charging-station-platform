#pragma once

#include "core/application/admin_repository.h"
#include "core/application/charging_repository.h"
#include "core/application/user_account_repository.h"

#include <map>
#include <mutex>

namespace ncs::core::application {

// Admin/ops state stored in process memory; station, charger, tariff, user
// and flow mutations delegate to the charging/account stores shared with the
// rest of the application, whatever adapter backs them.
class InMemoryAdminRepository final : public AdminRepository {
public:
  InMemoryAdminRepository(ChargingRepository &charging,
                          UserAccountRepository &accounts, bool withDemoAdmin);

  void withTransaction(const std::function<void()> &work) override;
  std::optional<AdminAccount>
  findAdminByUsername(std::string_view username) override;
  std::optional<AdminAccount> findAdminById(std::int64_t id) override;
  AdminUserPage listManagedUsers(const AdminUserQuery &query) override;
  std::optional<UserAccount> findManagedUser(std::int64_t id) override;
  AccountWriteResult updateManagedUserStatus(
      std::int64_t actorAdminId, std::int64_t userId, int status,
      std::string_view reason, std::int64_t expectedVersion, std::int64_t at,
      UserAccount &updated) override;
  void addAuditEvent(const AuditEvent &event) override;
  std::vector<AuditEvent> auditEvents(const AuditEventQuery &query) override;

  bool stationCodeExists(const std::string &code) override;
  bool createStationWithChargers(Station &station,
                                 const InitialChargerSpec &spec) override;
  bool saveStation(const Station &station) override;
  std::vector<Station> stations(std::optional<int> status,
                                std::optional<std::string> adcode,
                                const std::string &keyword) override;
  bool addChargers(std::vector<Charger> &chargers) override;
  void addTariffVersion(const RegionTariff &tariff) override;
  std::vector<RegionTariff>
  tariffVersions(std::optional<std::string> adcode) override;
  bool tariffOverlaps(const std::string &adcode, std::int64_t from,
                      std::int64_t to) override;
  std::int64_t addPriceAdjustment(const PriceAdjustment &adjustment) override;
  std::optional<PriceAdjustment>
  effectivePriceAdjustment(std::int64_t stationId, int chargerType,
                           std::int64_t at) override;
  void addDeviceCommand(const DeviceCommand &command) override;
  std::optional<DeviceCommand>
  deviceCommand(const std::string &commandNo) override;
  void saveDeviceCommand(const DeviceCommand &command) override;
  std::vector<DeviceCommand> dueDeviceCommands(std::int64_t now) override;
  std::optional<ChargingFlow> activeFlowOnCharger(std::int64_t chargerId) override;
  bool stationHasActiveFlow(std::int64_t stationId) override;
  AdminFlowPage flows(const AdminFlowQuery &query) override;
  std::vector<ChargingOrder> settledOrders(
      std::int64_t fromAt, std::int64_t toAt,
      std::optional<std::int64_t> stationId) override;
  std::optional<MlTask> runningMlTask(const std::string &taskType) override;
  std::vector<MlTask> overdueMlTasks(std::int64_t trainDeadline,
                                     std::int64_t predictDeadline) override;
  void addMlTask(const MlTask &task) override;
  std::optional<MlTask> mlTask(const std::string &taskNo) override;
  void saveMlTask(const MlTask &task) override;
  bool tryFinishMlTask(const MlTask &task, bool allowTimedOut) override;
  void addBackup(const BackupRecord &record) override;
  std::vector<BackupRecord> backups() override;
  std::optional<BackupRecord> backup(const std::string &backupNo) override;
  void saveBackup(const BackupRecord &record) override;
  bool createBackupSnapshot(BackupRecord &record) override;
  bool verifyBackupSnapshot(const BackupRecord &record) override;
  void cleanupAdminRecords(std::int64_t now) override;

private:
  void seedDemoAdmin();
  void pruneBackupsLocked(std::int64_t now);

  ChargingRepository &charging_;
  UserAccountRepository &accounts_;
  mutable std::recursive_mutex mutex_;
  std::map<std::string, AdminAccount> admins_;
  std::int64_t nextAdminId_ = 1;
  std::vector<AuditEvent> audit_;
  std::int64_t nextAdjustmentId_ = 1;
  std::vector<PriceAdjustment> adjustments_;
  std::map<std::string, DeviceCommand> deviceCommands_;
  std::map<std::string, MlTask> mlTasks_;
  std::map<std::string, BackupRecord> backups_;
};

} // namespace ncs::core::application
