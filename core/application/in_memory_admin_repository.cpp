#include "core/application/in_memory_admin_repository.h"

#include "core/application/security_crypto.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <stdexcept>

namespace ncs::core::application
{
namespace
{

bool isActiveStatus(int status)
{
    return isActiveFlowStatus(status);
}

bool matchesAudit(const AuditEvent& event, const AuditEventQuery& query)
{
    if (!query.actorId.empty() && std::to_string(event.actorAdminId) != query.actorId)
        return false;
    if (!query.action.empty() && event.action != query.action)
        return false;
    if (!query.targetType.empty() && event.targetType != query.targetType)
        return false;
    if (!query.targetId.empty() && event.targetId != query.targetId)
        return false;
    if (query.fromAt > 0 && event.at < query.fromAt)
        return false;
    if (query.toAt > 0 && event.at > query.toAt)
        return false;
    return true;
}

} // namespace

InMemoryAdminRepository::InMemoryAdminRepository(ChargingRepository& charging,
                                                 UserAccountRepository& accounts,
                                                 const bool withDemoAdmin)
    : charging_(charging), accounts_(accounts)
{
    if (withDemoAdmin)
        seedDemoAdmin();
}

void InMemoryAdminRepository::seedDemoAdmin()
{
    // SRS UC-A-01: development seed admin / 123456; production must not rely on
    // seeded credentials.
    AdminAccount admin;
    admin.id = nextAdminId_++;
    admin.username = "admin";
    admin.passwordHash = PasswordHasher().hash("123456", PasswordHasher::currentIterations, 6);
    admin.roles = {Role::Operator, Role::Owner};
    admins_.emplace(admin.username, admin);
}

void InMemoryAdminRepository::withTransaction(const std::function<void()>& work)
{
    std::lock_guard lock(mutex_);
    const auto admins = admins_;
    const auto nextAdminId = nextAdminId_;
    const auto audit = audit_;
    const auto adjustments = adjustments_;
    const auto commands = deviceCommands_;
    const auto tasks = mlTasks_;
    const auto backups = backups_;
    const auto nextAdjustmentId = nextAdjustmentId_;
    try
    {
        charging_.withTransaction(work);
    }
    catch (...)
    {
        admins_ = admins;
        nextAdminId_ = nextAdminId;
        audit_ = audit;
        adjustments_ = adjustments;
        deviceCommands_ = commands;
        mlTasks_ = tasks;
        backups_ = backups;
        nextAdjustmentId_ = nextAdjustmentId;
        throw;
    }
}

std::optional<AdminAccount>
InMemoryAdminRepository::findAdminByUsername(const std::string_view username)
{
    std::lock_guard lock(mutex_);
    const auto found = admins_.find(std::string(username));
    return found == admins_.end() ? std::nullopt : std::optional<AdminAccount>(found->second);
}

std::optional<AdminAccount> InMemoryAdminRepository::findAdminById(const std::int64_t id)
{
    std::lock_guard lock(mutex_);
    for (const auto& [username, admin] : admins_)
    {
        (void)username;
        if (admin.id == id)
            return admin;
    }
    return std::nullopt;
}

AdminUserPage InMemoryAdminRepository::listManagedUsers(const AdminUserQuery& query)
{
    std::vector<UserAccount> all = accounts_.listAccounts();
    std::vector<const UserAccount*> filtered;
    for (const auto& account : all)
    {
        if (account.deleted)
            continue;
        if (query.status && account.status != *query.status)
            continue;
        if (query.phoneExact && account.phone != *query.phoneExact)
            continue;
        if (query.phoneLast4 &&
            (account.phone.size() != 11 || account.phone.substr(7) != *query.phoneLast4))
            continue;
        filtered.push_back(&account);
    }
    if (query.sort == "registeredAt")
    {
        std::stable_sort(filtered.begin(), filtered.end(),
                         [](const UserAccount* left, const UserAccount* right)
                         { return left->registeredAt < right->registeredAt; });
    }
    else if (query.sort == "balanceCent")
    {
        std::stable_sort(filtered.begin(), filtered.end(),
                         [](const UserAccount* left, const UserAccount* right)
                         { return left->balanceCent < right->balanceCent; });
    }
    else
    {
        std::stable_sort(filtered.begin(), filtered.end(),
                         [](const UserAccount* left, const UserAccount* right)
                         { return left->registeredAt > right->registeredAt; });
    }
    AdminUserPage page;
    page.total = static_cast<int>(filtered.size());
    page.page = query.page;
    page.pageSize = query.pageSize;
    const std::size_t first = static_cast<std::size_t>(query.page - 1) * query.pageSize;
    for (std::size_t index = first;
         index < filtered.size() && page.items.size() < static_cast<std::size_t>(query.pageSize);
         ++index)
    {
        page.items.push_back(*filtered[index]);
    }
    return page;
}

std::optional<UserAccount> InMemoryAdminRepository::findManagedUser(const std::int64_t id)
{
    return accounts_.findById(id);
}

AccountWriteResult InMemoryAdminRepository::updateManagedUserStatus(
    const std::int64_t actorAdminId, const std::int64_t userId, const int status,
    const std::string_view reason, const std::int64_t expectedVersion, const std::int64_t at,
    UserAccount& updated)
{
    std::lock_guard lock(mutex_);
    const auto current = accounts_.findById(userId);
    if (!current || current->deleted)
        return AccountWriteResult::NotFound;
    if (current->version != expectedVersion)
        return AccountWriteResult::VersionConflict;
    AccountWriteResult write = accounts_.updateStatus(userId, status, updated);
    if (write == AccountWriteResult::Success)
    {
        audit_.push_back(AuditEvent{actorAdminId, status == 0 ? "USER_FROZEN" : "USER_UNFROZEN",
                                    "USER", std::to_string(userId), std::string(reason), at});
    }
    return write;
}

void InMemoryAdminRepository::addAuditEvent(const AuditEvent& event)
{
    std::lock_guard lock(mutex_);
    audit_.push_back(event);
}

std::vector<AuditEvent> InMemoryAdminRepository::auditEvents(const AuditEventQuery& query)
{
    std::lock_guard lock(mutex_);
    std::vector<AuditEvent> result;
    for (auto iterator = audit_.rbegin(); iterator != audit_.rend(); ++iterator)
    {
        if (!matchesAudit(*iterator, query))
            continue;
        result.push_back(*iterator);
    }
    const std::size_t first = static_cast<std::size_t>(query.page - 1) * query.pageSize;
    if (first >= result.size())
        return {};
    result.erase(result.begin(), result.begin() + static_cast<std::ptrdiff_t>(first));
    if (result.size() > static_cast<std::size_t>(query.pageSize))
        result.resize(static_cast<std::size_t>(query.pageSize));
    return result;
}

bool InMemoryAdminRepository::stationCodeExists(const std::string& code)
{
    return charging_.stationCodeExists(code);
}

bool InMemoryAdminRepository::createStationWithChargers(Station& station,
                                                        const InitialChargerSpec& spec)
{
    std::lock_guard lock(mutex_);
    std::vector<Charger> chargers;
    chargers.reserve(static_cast<std::size_t>(spec.count));
    for (int index = 1; index <= spec.count; ++index)
    {
        char suffix[8];
        std::snprintf(suffix, sizeof(suffix), "%02d", index);
        Charger charger;
        charger.stationId = station.id;
        charger.code =
            station.code + (spec.chargerType == ChargerType::DcFast ? "-DC-" : "-AC-") + suffix;
        charger.type = spec.chargerType;
        charger.powerWatt = spec.powerWatt;
        charger.connectorStandard = spec.connectorStandard;
        chargers.push_back(charger);
    }
    for (const auto& charger : chargers)
    {
        if (charging_.chargerCodeExists(charger.code))
            return false;
    }
    if (!charging_.addStation(station))
        return false;
    for (auto& charger : chargers)
    {
        charger.stationId = station.id;
        if (!charging_.addCharger(charger))
            return false;
    }
    return true;
}

bool InMemoryAdminRepository::saveStation(const Station& station)
{
    if (!charging_.station(station.id))
        return false;
    charging_.saveStation(station);
    return true;
}

std::vector<Station> InMemoryAdminRepository::stations(const std::optional<int> status,
                                                       const std::optional<std::string> adcode,
                                                       const std::string& keyword)
{
    std::vector<Station> result;
    for (Station& station : charging_.stations())
    {
        if (status && station.enabled != (*status == 1))
            continue;
        if (adcode && station.adcode != *adcode)
            continue;
        if (!keyword.empty() && station.name.find(keyword) == std::string::npos &&
            station.address.find(keyword) == std::string::npos && station.code != keyword)
            continue;
        result.push_back(station);
    }
    return result;
}

bool InMemoryAdminRepository::addChargers(std::vector<Charger>& chargers)
{
    std::lock_guard lock(mutex_);
    for (std::size_t outer = 0; outer < chargers.size(); ++outer)
    {
        if (charging_.chargerCodeExists(chargers[outer].code))
            return false;
        for (std::size_t inner = 0; inner < outer; ++inner)
        {
            if (chargers[outer].code == chargers[inner].code)
                return false;
        }
    }
    for (auto& charger : chargers)
    {
        if (!charging_.addCharger(charger))
            return false;
    }
    return true;
}

void InMemoryAdminRepository::addTariffVersion(const RegionTariff& tariff)
{
    charging_.addTariff(tariff);
}

std::vector<RegionTariff> InMemoryAdminRepository::tariffVersions(std::optional<std::string> adcode)
{
    return charging_.tariffVersions(std::move(adcode));
}

bool InMemoryAdminRepository::tariffOverlaps(const std::string& adcode, const std::int64_t from,
                                             const std::int64_t to)
{
    for (const auto& tariff : charging_.tariffVersions(adcode))
    {
        if (tariff.adcode != adcode)
            continue;
        if (from <= tariff.effectiveTo && to >= tariff.effectiveFrom)
            return true;
    }
    return false;
}

std::int64_t InMemoryAdminRepository::addPriceAdjustment(const PriceAdjustment& adjustment)
{
    std::lock_guard lock(mutex_);
    adjustments_.push_back(adjustment);
    adjustments_.back().id = nextAdjustmentId_++;
    return adjustments_.back().id;
}

std::optional<PriceAdjustment>
InMemoryAdminRepository::effectivePriceAdjustment(const std::int64_t stationId,
                                                  const int chargerType, const std::int64_t at)
{
    std::lock_guard lock(mutex_);
    const PriceAdjustment* best = nullptr;
    for (const auto& adjustment : adjustments_)
    {
        if (adjustment.stationId != stationId || adjustment.chargerType != chargerType)
            continue;
        if (at < adjustment.effectiveFrom || at > adjustment.effectiveTo)
            continue;
        if (!best || adjustment.id > best->id)
            best = &adjustment;
    }
    return best ? std::optional<PriceAdjustment>(*best) : std::nullopt;
}

void InMemoryAdminRepository::addDeviceCommand(const DeviceCommand& command)
{
    std::lock_guard lock(mutex_);
    deviceCommands_.emplace(command.commandNo, command);
}

std::optional<DeviceCommand> InMemoryAdminRepository::deviceCommand(const std::string& commandNo)
{
    std::lock_guard lock(mutex_);
    const auto found = deviceCommands_.find(commandNo);
    return found == deviceCommands_.end() ? std::nullopt
                                          : std::optional<DeviceCommand>(found->second);
}

void InMemoryAdminRepository::saveDeviceCommand(const DeviceCommand& command)
{
    std::lock_guard lock(mutex_);
    deviceCommands_[command.commandNo] = command;
}

std::vector<DeviceCommand> InMemoryAdminRepository::dueDeviceCommands(const std::int64_t now)
{
    std::lock_guard lock(mutex_);
    std::vector<DeviceCommand> due;
    for (const auto& [commandNo, command] : deviceCommands_)
    {
        (void)commandNo;
        if (command.status == "PENDING" && command.executeAt <= now)
            due.push_back(command);
    }
    return due;
}

std::optional<ChargingFlow>
InMemoryAdminRepository::activeFlowOnCharger(const std::int64_t chargerId)
{
    for (const auto& flow : charging_.allFlows())
    {
        if (flow.chargerId && *flow.chargerId == chargerId && isActiveStatus(flow.status))
            return flow;
    }
    return std::nullopt;
}

bool InMemoryAdminRepository::stationHasActiveFlow(const std::int64_t stationId)
{
    for (const auto& flow : charging_.allFlows())
    {
        if (flow.stationId == stationId && isActiveStatus(flow.status))
            return true;
    }
    return false;
}

AdminFlowPage InMemoryAdminRepository::flows(const AdminFlowQuery& query)
{
    std::vector<ChargingFlow> matching;
    for (const auto& flow : charging_.allFlows())
    {
        if (query.status && flow.status != *query.status)
            continue;
        if (query.stationId && flow.stationId != *query.stationId)
            continue;
        if (query.chargerId && flow.chargerId != *query.chargerId)
            continue;
        if (query.userId && flow.userId != *query.userId)
            continue;
        matching.push_back(flow);
    }
    std::sort(matching.begin(), matching.end(),
              [](const ChargingFlow& left, const ChargingFlow& right)
              {
                  if (left.createdAt != right.createdAt)
                      return left.createdAt > right.createdAt;
                  return left.flowNo > right.flowNo;
              });
    AdminFlowPage page;
    page.total = static_cast<int>(matching.size());
    page.page = query.page;
    page.pageSize = query.pageSize;
    const std::size_t first = static_cast<std::size_t>(query.page - 1) * query.pageSize;
    for (std::size_t index = first;
         index < matching.size() && page.items.size() < static_cast<std::size_t>(query.pageSize);
         ++index)
    {
        page.items.push_back(std::move(matching[index]));
    }
    return page;
}

std::vector<ChargingOrder>
InMemoryAdminRepository::settledOrders(const std::int64_t fromAt, const std::int64_t toAt,
                                       const std::optional<std::int64_t> stationId)
{
    std::vector<ChargingOrder> result;
    for (const auto& order : charging_.allOrders())
    {
        if (order.status != static_cast<int>(FlowStatus::Completed) || !order.settledAt)
            continue;
        if (order.settledAt < fromAt)
            continue;
        if (toAt > 0 && order.settledAt > toAt)
            continue;
        if (stationId && order.stationId != *stationId)
            continue;
        result.push_back(order);
    }
    return result;
}

std::optional<MlTask> InMemoryAdminRepository::runningMlTask(const std::string& taskType)
{
    std::lock_guard lock(mutex_);
    for (const auto& [taskNo, task] : mlTasks_)
    {
        (void)taskNo;
        if (task.taskType == taskType && (task.status == "PENDING" || task.status == "RUNNING"))
            return task;
    }
    return std::nullopt;
}

std::vector<MlTask> InMemoryAdminRepository::overdueMlTasks(const std::int64_t trainDeadline,
                                                            const std::int64_t predictDeadline)
{
    std::lock_guard lock(mutex_);
    std::vector<MlTask> overdue;
    for (const auto& [taskNo, task] : mlTasks_)
    {
        (void)taskNo;
        if (task.status != "PENDING" && task.status != "RUNNING")
            continue;
        const std::int64_t deadline = task.taskType == "TRAIN" ? trainDeadline : predictDeadline;
        if (task.createdAt <= deadline)
            overdue.push_back(task);
    }
    return overdue;
}

void InMemoryAdminRepository::addMlTask(const MlTask& task)
{
    std::lock_guard lock(mutex_);
    if (!mlTasks_.emplace(task.taskNo, task).second)
        throw std::runtime_error("ml task already exists");
}

std::optional<MlTask> InMemoryAdminRepository::mlTask(const std::string& taskNo)
{
    std::lock_guard lock(mutex_);
    const auto found = mlTasks_.find(taskNo);
    return found == mlTasks_.end() ? std::nullopt : std::optional<MlTask>(found->second);
}

void InMemoryAdminRepository::saveMlTask(const MlTask& task)
{
    std::lock_guard lock(mutex_);
    mlTasks_[task.taskNo] = task;
}

bool InMemoryAdminRepository::tryFinishMlTask(const MlTask& task, const bool allowTimedOut)
{
    std::lock_guard lock(mutex_);
    const auto found = mlTasks_.find(task.taskNo);
    if (found == mlTasks_.end())
        return false;
    const bool live = found->second.status == "PENDING" || found->second.status == "RUNNING";
    if (!live && !(allowTimedOut && found->second.status == "TIMED_OUT"))
        return false;
    found->second = task;
    return true;
}

void InMemoryAdminRepository::addBackup(const BackupRecord& record)
{
    std::lock_guard lock(mutex_);
    backups_.emplace(record.backupNo, record);
}

std::vector<BackupRecord> InMemoryAdminRepository::backups()
{
    std::lock_guard lock(mutex_);
    std::vector<BackupRecord> result;
    for (const auto& [backupNo, record] : backups_)
    {
        (void)backupNo;
        result.push_back(record);
    }
    std::reverse(result.begin(), result.end());
    return result;
}

std::optional<BackupRecord> InMemoryAdminRepository::backup(const std::string& backupNo)
{
    std::lock_guard lock(mutex_);
    const auto found = backups_.find(backupNo);
    return found == backups_.end() ? std::nullopt : std::optional<BackupRecord>(found->second);
}

void InMemoryAdminRepository::saveBackup(const BackupRecord& record)
{
    std::lock_guard lock(mutex_);
    backups_[record.backupNo] = record;
}

bool InMemoryAdminRepository::createBackupSnapshot(BackupRecord& record)
{
    // This adapter is used by isolated application tests only. Production uses
    // SqliteRepository, whose implementation creates a real Online Backup.
    record.checksum = sha256Hex(record.backupNo);
    record.sizeBytes = 4096;
    return true;
}

bool InMemoryAdminRepository::verifyBackupSnapshot(const BackupRecord& record)
{
    return !record.checksum.empty() && record.sizeBytes > 0;
}

void InMemoryAdminRepository::cleanupAdminRecords(const std::int64_t now)
{
    constexpr std::int64_t retention = 180LL * 24 * 3600;
    std::lock_guard lock(mutex_);
    audit_.erase(std::remove_if(audit_.begin(), audit_.end(),
                                [=](const AuditEvent& event)
                                { return event.at < now - retention; }),
                 audit_.end());
    for (auto iterator = deviceCommands_.begin(); iterator != deviceCommands_.end();)
    {
        if (iterator->second.completedAt && *iterator->second.completedAt < now - retention)
        {
            iterator = deviceCommands_.erase(iterator);
        }
        else
        {
            ++iterator;
        }
    }
    pruneBackupsLocked(now);
}

void InMemoryAdminRepository::pruneBackupsLocked(const std::int64_t now)
{
    // NFR-R-03: keep the newest backup of each of the last seven days plus the
    // newest of each of the last four weeks; failed diagnostics age out after a
    // week.
    std::vector<BackupRecord> ordered;
    ordered.reserve(backups_.size());
    for (const auto& [backupNo, record] : backups_)
    {
        (void)backupNo;
        ordered.push_back(record);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const BackupRecord& left, const BackupRecord& right)
              {
                  if (left.createdAt != right.createdAt)
                      return left.createdAt > right.createdAt;
                  return left.backupNo > right.backupNo;
              });
    constexpr std::int64_t day = 24 * 3600;
    std::set<std::int64_t> keptDays;
    std::set<std::int64_t> keptWeeks;
    for (const auto& record : ordered)
    {
        if (record.status != "SUCCEEDED")
        {
            if (record.createdAt < now - 7 * day)
                backups_.erase(record.backupNo);
            continue;
        }
        const std::int64_t dayBucket = record.createdAt / day;
        const std::int64_t weekBucket = record.createdAt / (7 * day);
        if (keptDays.count(dayBucket) == 0 && keptDays.size() < 7)
        {
            keptDays.insert(dayBucket);
            keptWeeks.insert(weekBucket);
            continue;
        }
        if (keptWeeks.count(weekBucket) == 0 && keptWeeks.size() < 4)
        {
            keptWeeks.insert(weekBucket);
            continue;
        }
        backups_.erase(record.backupNo);
    }
}

} // namespace ncs::core::application
