#pragma once

#include "core/application/charging_repository.h"
#include "core/application/session_manager.h"
#include "core/application/user_account_repository.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ncs::core::application
{

struct AdminAccount
{
    std::int64_t id = 0;
    std::string username;
    std::string passwordHash;
    int status = 1;
    std::vector<Role> roles;
    bool mustChangePassword = false;
    std::int64_t version = 1;
};

struct AdminUserQuery
{
    std::optional<int> status;
    std::optional<std::string> phoneExact;
    std::optional<std::string> phoneLast4;
    std::string sort;
    int page = 1;
    int pageSize = 20;
};

// UC-A-09 管理员账号管理: write results shared by every admin-account
// mutation. HashMismatch means the compare-and-swap failed because the stored
// credential digest changed after the caller verified the current password.
enum class AdminAccountWriteResult
{
    Success,
    NotFound,
    UsernameExists,
    VersionConflict,
    HashMismatch,
};

struct AdminAccountQuery
{
    int page = 1;
    int pageSize = 20;
};

struct AdminAccountPage
{
    std::vector<AdminAccount> items;
    int total = 0;
    int page = 1;
    int pageSize = 20;
};

struct AdminUserPage
{
    std::vector<UserAccount> items;
    int total = 0;
    int page = 1;
    int pageSize = 20;
};

struct AuditEvent
{
    std::int64_t actorAdminId = 0;
    std::string action;
    std::string targetType;
    std::string targetId;
    std::string reason;
    std::int64_t at = 0;
};

struct AuditEventQuery
{
    std::string actorId;
    std::string action;
    std::string targetType;
    std::string targetId;
    std::int64_t fromAt = 0;
    std::int64_t toAt = 0;
    int page = 1;
    int pageSize = 20;
};

struct AdminFlowQuery
{
    std::optional<int> status;
    std::optional<std::int64_t> stationId;
    std::optional<std::int64_t> chargerId;
    std::optional<std::int64_t> userId;
    int page = 1;
    int pageSize = 20;
};

struct AdminFlowPage
{
    std::vector<ChargingFlow> items;
    int total = 0;
    int page = 1;
    int pageSize = 20;
};

// Admin/ops state owned outside the user-facing charging store: price
// adjustments, simulated device commands, ML task orchestration records and
// backup records. Station/charger/tariff writes go through the charging store
// so both adapters stay the single source of truth for those rows.
struct PriceAdjustment
{
    std::int64_t id = 0;
    std::int64_t stationId = 0;
    int chargerType = 1;
    std::string source;
    int adjustmentBp = 0;
    std::int64_t effectiveFrom = 0;
    std::int64_t effectiveTo = 0;
    std::string reason;
};

struct DeviceCommand
{
    std::string commandNo;
    std::int64_t chargerId = 0;
    std::string chargerCode;
    std::string status = "PENDING";
    std::string reason;
    std::string actorId;
    std::int64_t createdAt = 0;
    std::int64_t executeAt = 0;
    std::optional<std::int64_t> completedAt;
    std::string errorSummary;
};

struct MlTask
{
    std::string taskNo;
    std::string taskType;
    std::string status = "PENDING";
    std::string modelVersion;
    std::vector<int> horizonHours;
    std::int64_t createdAt = 0;
    std::optional<std::int64_t> finishedAt;
    std::string metricsSummary;
    std::string errorSummary;
};

struct BackupRecord
{
    std::string backupNo;
    std::string status = "PENDING";
    std::string checksum;
    std::int64_t sizeBytes = 0;
    std::int64_t createdAt = 0;
    std::string verificationStatus;
    std::optional<std::int64_t> verifiedAt;
    // Internal storage location. Controllers must never serialize this field.
    std::string storagePath;
};

struct InitialChargerSpec
{
    int count = 0;
    ChargerType chargerType = ChargerType::DcFast;
    std::int64_t powerWatt = 0;
    std::string connectorStandard = "GB/T";
};

class AdminRepository
{
  public:
    virtual ~AdminRepository() = default;
    virtual void withTransaction(const std::function<void()>& work) = 0;
    // Coherent read snapshot without reserving SQLite's single writer slot.
    virtual void withReadTransaction(const std::function<void()>& work)
    {
        work();
    }
    virtual std::optional<AdminAccount> findAdminByUsername(std::string_view username) = 0;
    virtual std::optional<AdminAccount> findAdminById(std::int64_t id) = 0;
    // UC-A-09: admin-account management. Mutations write their audit event in
    // the same transaction; created accounts are OPERATOR, enabled, non-demo and
    // flagged for a first-login password change.
    virtual AdminAccountPage adminAccounts(const AdminAccountQuery& query) = 0;
    virtual AdminAccountWriteResult createAdminAccount(std::int64_t actorAdminId,
                                                       std::string_view username,
                                                       std::string_view passwordHash,
                                                       std::string_view reason, std::int64_t at,
                                                       AdminAccount& created) = 0;
    virtual AdminAccountWriteResult
    updateAdminAccountStatus(std::int64_t actorAdminId, std::int64_t adminId, int status,
                             std::string_view reason, std::int64_t expectedVersion, std::int64_t at,
                             AdminAccount& updated) = 0;
    virtual AdminAccountWriteResult changeAdminAccountPassword(
        std::int64_t actorAdminId, std::int64_t adminId, std::string_view expectedCurrentHash,
        std::string_view newPasswordHash, std::int64_t at, AdminAccount& updated) = 0;
    virtual AdminUserPage listManagedUsers(const AdminUserQuery& query) = 0;
    virtual std::optional<UserAccount> findManagedUser(std::int64_t id) = 0;
    virtual AccountWriteResult updateManagedUserStatus(std::int64_t actorAdminId,
                                                       std::int64_t userId, int status,
                                                       std::string_view reason,
                                                       std::int64_t expectedVersion,
                                                       std::int64_t at, UserAccount& updated) = 0;
    virtual void addAuditEvent(const AuditEvent& event) = 0;
    virtual std::vector<AuditEvent> auditEvents(const AuditEventQuery& query) = 0;

    virtual bool stationCodeExists(const std::string& code) = 0;
    virtual bool createStationWithChargers(Station& station, const InitialChargerSpec& spec) = 0;
    virtual bool saveStation(const Station& station) = 0;
    virtual std::vector<Station> stations(std::optional<int> status,
                                          std::optional<std::string> adcode,
                                          const std::string& keyword) = 0;
    virtual bool addChargers(std::vector<Charger>& chargers) = 0;
    virtual void addTariffVersion(const RegionTariff& tariff) = 0;
    virtual std::vector<RegionTariff> tariffVersions(std::optional<std::string> adcode) = 0;
    virtual bool tariffOverlaps(const std::string& adcode, std::int64_t from, std::int64_t to) = 0;
    virtual std::int64_t addPriceAdjustment(const PriceAdjustment& adjustment) = 0;
    virtual std::optional<PriceAdjustment>
    effectivePriceAdjustment(std::int64_t stationId, int chargerType, std::int64_t at) = 0;
    virtual void addDeviceCommand(const DeviceCommand& command) = 0;
    virtual std::optional<DeviceCommand> deviceCommand(const std::string& commandNo) = 0;
    virtual void saveDeviceCommand(const DeviceCommand& command) = 0;
    virtual std::vector<DeviceCommand> dueDeviceCommands(std::int64_t now) = 0;
    virtual std::optional<ChargingFlow> activeFlowOnCharger(std::int64_t chargerId) = 0;
    virtual bool stationHasActiveFlow(std::int64_t stationId) = 0;
    virtual AdminFlowPage flows(const AdminFlowQuery& query) = 0;
    virtual std::vector<ChargingOrder> settledOrders(std::int64_t fromAt, std::int64_t toAt,
                                                     std::optional<std::int64_t> stationId) = 0;
    virtual std::optional<MlTask> runningMlTask(const std::string& taskType) = 0;
    virtual std::vector<MlTask> overdueMlTasks(std::int64_t trainDeadline,
                                               std::int64_t predictDeadline) = 0;
    virtual void addMlTask(const MlTask& task) = 0;
    virtual std::optional<MlTask> mlTask(const std::string& taskNo) = 0;
    virtual void saveMlTask(const MlTask& task) = 0;
    // Terminal-state compare-and-swap. By default only a live task can finish;
    // allowTimedOut is reserved for a completion request received on time but
    // processed just after the timeout worker won the race.
    virtual bool tryFinishMlTask(const MlTask& task, bool allowTimedOut) = 0;
    virtual void addBackup(const BackupRecord& record) = 0;
    virtual std::vector<BackupRecord> backups() = 0;
    virtual std::optional<BackupRecord> backup(const std::string& backupNo) = 0;
    virtual void saveBackup(const BackupRecord& record) = 0;
    virtual bool createBackupSnapshot(BackupRecord& record) = 0;
    virtual bool verifyBackupSnapshot(const BackupRecord& record) = 0;
    virtual void cleanupAdminRecords(std::int64_t now) = 0;
};

std::string roleName(Role role);

} // namespace ncs::core::application
