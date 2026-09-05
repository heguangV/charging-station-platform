#include "core/application/admin_ops_service.h"
#include "core/application/admin_station_service.h"
#include "core/application/business_numbers.h"
#include "core/application/charge_flow_service.h"
#include "core/application/idempotency_service.h"
#include "core/application/station_service.h"
#include "core/application/wallet_service.h"
#include "infrastructure/sqlite/sqlite_repository.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <sqlite3.h>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace
{

std::int64_t processId()
{
#if defined(_WIN32)
    return static_cast<std::int64_t>(::_getpid());
#else
    return static_cast<std::int64_t>(::getpid());
#endif
}

class TestRunner final
{
  public:
    void check(const bool condition, const std::string_view message)
    {
        if (!condition)
        {
            std::cerr << "FAIL: " << message << '\n';
            ++failures_;
        }
    }
    int result() const
    {
        return failures_ == 0 ? 0 : 1;
    }

  private:
    int failures_ = 0;
};

class TemporaryDatabase final
{
  public:
    TemporaryDatabase()
        : path_(std::filesystem::temp_directory_path() /
                ("ncs-sqlite-" + std::to_string(processId()) + ".db"))
    {
        cleanup();
    }

    ~TemporaryDatabase()
    {
        cleanup();
    }

    std::string path() const
    {
        return path_.string();
    }

  private:
    void cleanup() const
    {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(path_.string() + "-wal", ignored);
        std::filesystem::remove(path_.string() + "-shm", ignored);
        std::filesystem::remove_all(path_.parent_path() / (path_.filename().string() + ".backups"),
                                    ignored);
    }

    std::filesystem::path path_;
};

void executeSql(const std::string& path, const char* sql)
{
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK)
    {
        throw std::runtime_error("test database open failed");
    }
    char* error = nullptr;
    const int result = sqlite3_exec(database, sql, nullptr, nullptr, &error);
    const std::string message = error ? error : "test SQL failed";
    sqlite3_free(error);
    sqlite3_close(database);
    if (result != SQLITE_OK)
        throw std::runtime_error(message);
}

std::int64_t queryInteger(const std::string& path, const char* sql)
{
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    {
        throw std::runtime_error("test database open failed");
    }
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK)
    {
        const std::string message = sqlite3_errmsg(database);
        sqlite3_close(database);
        throw std::runtime_error(message);
    }
    const std::int64_t value =
        sqlite3_step(statement) == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : -1;
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return value;
}

} // namespace

int main()
{
    using namespace ncs::core::application;
    using ncs::infrastructure::sqlite::SqliteRepository;

    TestRunner tests;
    TemporaryDatabase database;
    const auto now = std::chrono::system_clock::time_point(std::chrono::seconds(1788500000));
    std::int64_t userId = 0;
    std::string flowNo;
    std::string orderNo;
    std::string backupNo;
    std::string commandNo;
    constexpr std::string_view idempotencyKey = "2cb640c6-6995-4be5-9161-f0e2c1210099";
    constexpr std::string_view rollbackIdempotencyKey = "2cb640c6-6995-4be5-9161-f0e2c1210098";

    {
        SqliteRepository repository(database.path());
        repository.ensureDevelopmentAdmin(true);
        tests.check(repository.check().ready(),
                    "fresh database is migrated, writable and in WAL mode");
        tests.check(repository.findAdminByUsername("admin").has_value() &&
                        queryInteger(database.path(), "SELECT MAX(version) FROM schema_version") ==
                            8,
                    "demo-seed migration, development account and full history exist");

        UserAccount account;
        account.username = "persistent_user";
        account.phone = "13800139999";
        account.nickname = "持久化用户";
        account.registeredAt = 1788500000;
        tests.check(repository.create(account) == AccountWriteResult::Success,
                    "user and wallet are created atomically");
        userId = account.id;

        AdminUserQuery userQuery;
        userQuery.phoneLast4 = "9999";
        userQuery.sort = "balanceCent";
        const auto userPage = repository.listManagedUsers(userQuery);
        tests.check(userPage.total == 1 && userPage.items.size() == 1 &&
                        userPage.items.front().id == userId &&
                        userPage.items.front().phone == "13800139999",
                    "admin user list filters and sorts in SQL");
        AdminUserQuery userStatusQuery;
        userStatusQuery.status = 0;
        tests.check(repository.listManagedUsers(userStatusQuery).total == 0,
                    "admin user list applies status filter in SQL");

        BusinessNumbers numbers(&repository);
        WalletService wallet(repository, repository, numbers);
        ChargeFlowService flows(repository, repository, repository, numbers, 60);
        tests.check(wallet.recharge(userId, 10000, now).ok(), "wallet recharge is persisted");

        auto station = *repository.station(1);
        Station firstUpdate = station;
        firstUpdate.name = "并发更新一";
        ++firstUpdate.version;
        Station staleUpdate = firstUpdate;
        staleUpdate.name = "并发更新二";
        bool firstSaved = false;
        bool staleSaved = true;
        repository.withTransaction([&] { firstSaved = repository.saveStation(firstUpdate); });
        repository.withTransaction([&] { staleSaved = repository.saveStation(staleUpdate); });
        tests.check(firstSaved && !staleSaved && repository.station(1)->version == 2 &&
                        repository.station(1)->name == "并发更新一",
                    "station optimistic version prevents stale overwrite");

        PriceAdjustment adjustment;
        adjustment.stationId = 1;
        adjustment.chargerType = 1;
        adjustment.source = "MANUAL";
        adjustment.adjustmentBp = 500;
        adjustment.effectiveFrom = 1788400000;
        adjustment.effectiveTo = 1788600000;
        adjustment.reason = "持久化调价测试";
        const auto adjustmentId = repository.addPriceAdjustment(adjustment);
        const auto lookup = [&repository](const std::int64_t stationId, const int chargerType,
                                          const std::int64_t at)
        {
            const auto active = repository.effectivePriceAdjustment(stationId, chargerType, at);
            return active ? active->adjustmentBp : 0LL;
        };
        class NoopGeocoder final : public Geocoder
        {
          public:
            std::optional<Location> resolve(const std::string&) override
            {
                return std::nullopt;
            }
        } geocoder;
        StationService stationService(repository, geocoder, lookup);
        const auto adjustedQuote = stationService.stationQuote(1, ChargerType::DcFast, now);
        tests.check(adjustmentId > 0 && adjustedQuote.ok() &&
                        adjustedQuote.value->mlAdjustmentBp == 500,
                    "persisted price adjustment reaches user quote calculation");

        AdminOpsService adminOps(repository, repository, flows, numbers);
        ServiceResult<BackupRecord> backup;
        repository.withTransaction([&] { backup = adminOps.createBackup(1, now); });
        tests.check(backup.ok() && backup.value->status == "SUCCEEDED" &&
                        backup.value->sizeBytes > 0 && !backup.value->checksum.empty(),
                    "SQLite Online Backup creates a real snapshot");
        if (backup.ok())
        {
            backupNo = backup.value->backupNo;
            const auto verified = adminOps.verifyBackup(1, backupNo, now);
            tests.check(verified.ok() && verified.value->verificationStatus == "SUCCEEDED",
                        "backup verifies from an isolated copy");
        }

        AdminStationService adminStations(repository, repository, flows, numbers);
        const auto restart = adminStations.createRestartCommand(1, 3, "持久化命令测试", now);
        tests.check(restart.ok() && repository.charger(3)->status == ChargerStatus::Restarting,
                    "restart command atomically reserves the charger as restarting");
        if (restart.ok())
            commandNo = restart.value->commandNo;

        try
        {
            repository.withTransaction(
                [&]
                {
                    repository.addAuditEvent(
                        AuditEvent{1, "ROLLBACK_TEST", "TEST", "1", {}, 1788500000});
                    throw std::runtime_error("injected admin transaction failure");
                });
        }
        catch (const std::runtime_error&)
        {
        }
        AuditEventQuery rollbackAudit;
        rollbackAudit.action = "ROLLBACK_TEST";
        tests.check(repository.auditEvents(rollbackAudit).empty(),
                    "admin audit entry rolls back with its business transaction");

        IdempotencyService atomicIdempotency(&repository);
        const auto rollbackReservation = atomicIdempotency.begin(
            "atomic-recharge", rollbackIdempotencyKey, "{\"amountCent\":100}", now, true);
        {
            IdempotencyLease lease(atomicIdempotency, "atomic-recharge",
                                   std::string(rollbackIdempotencyKey),
                                   *rollbackReservation.leaseToken);
            StoredHttpResult ignored;
            try
            {
                lease.executeAndComplete(
                    [&]() -> StoredHttpResult
                    {
                        const auto transientRecharge =
                            wallet.recharge(userId, 100, now + std::chrono::seconds(1));
                        if (!transientRecharge.ok())
                            throw std::runtime_error("unexpected recharge failure");
                        throw std::runtime_error("injected response persistence failure");
                    },
                    ignored, now + std::chrono::seconds(1));
            }
            catch (const std::runtime_error&)
            {
            }
        }
        const auto rollbackRetry =
            atomicIdempotency.begin("atomic-recharge", rollbackIdempotencyKey,
                                    "{\"amountCent\":100}", now + std::chrono::seconds(2), true);
        tests.check(repository.wallet(userId).balanceCent == 10000 &&
                        rollbackRetry.decision == IdempotencyDecision::Proceed,
                    "business write and idempotency completion roll back together");
        if (rollbackRetry.leaseToken)
        {
            atomicIdempotency.abort("atomic-recharge", rollbackIdempotencyKey,
                                    *rollbackRetry.leaseToken);
        }

        const auto created = flows.createFlow(userId, 1, 1, std::nullopt, now);
        tests.check(created.ok() && created.value->quote, "persistent flow receives a quote");
        flowNo = created.value->flowNo;
        const auto confirmed =
            flows.confirmQuote(userId, flowNo, created.value->quote->quoteNo,
                               created.value->version, now + std::chrono::seconds(1));
        tests.check(confirmed.ok(), "persistent quote confirmation creates an order");
        orderNo = confirmed.value->orderNo;
        const auto started = flows.start(userId, flowNo, confirmed.value->version, std::nullopt,
                                         std::nullopt, now + std::chrono::seconds(2));
        tests.check(started.ok(), "persistent flow enters charging");

        AdminFlowQuery flowQuery;
        flowQuery.userId = userId;
        const auto flowPage = repository.flows(flowQuery);
        tests.check(flowPage.total == 1 && flowPage.items.size() == 1 &&
                        flowPage.items.front().flowNo == flowNo &&
                        flowPage.items.front().status == 40,
                    "admin flow listing filters and paginates in SQL");
        AdminFlowQuery flowSecondPage = flowQuery;
        flowSecondPage.page = 2;
        tests.check(repository.flows(flowSecondPage).items.empty(),
                    "admin flow listing honors the page offset");
        const auto activeCharger = repository.flow(flowNo)->chargerId;
        tests.check(activeCharger &&
                        repository.activeFlowOnCharger(*activeCharger)->flowNo == flowNo &&
                        repository.stationHasActiveFlow(1),
                    "active flow lookups use targeted SQL queries");

        const auto originalWallet = repository.wallet(userId);
        try
        {
            repository.withTransaction(
                [&]
                {
                    auto changed = originalWallet;
                    changed.balanceCent = 1;
                    repository.saveWallet(changed);
                    throw std::runtime_error("injected transaction failure");
                });
        }
        catch (const std::runtime_error&)
        {
        }
        tests.check(repository.wallet(userId).balanceCent == originalWallet.balanceCent,
                    "failed transaction rolls back all writes");

        IdempotencyService idempotency(&repository);
        const auto reservation =
            idempotency.begin("u1:recharge", idempotencyKey, "{\"amountCent\":10000}", now, true);
        IdempotencyLease lease(idempotency, "u1:recharge", std::string(idempotencyKey),
                               *reservation.leaseToken);
        tests.check(reservation.decision == IdempotencyDecision::Proceed &&
                        lease.complete(StoredHttpResult{200, "application/json", "persisted"}, now),
                    "completed idempotency response is persisted");
    }

    {
        SqliteRepository repository(database.path());
        repository.ensureDevelopmentAdmin(true);
        BusinessNumbers numbers(&repository);
        ChargeFlowService flows(repository, repository, repository, numbers, 60);
        AdminStationService adminStations(repository, repository, flows, numbers);
        adminStations.completeDueCommands(now + std::chrono::seconds(3));
        IdempotencyService idempotency(&repository);
        const auto replay =
            idempotency.begin("u1:recharge", idempotencyKey, "{\"amountCent\":10000}", now, true);
        tests.check(replay.decision == IdempotencyDecision::Replay && replay.replay &&
                        replay.replay->body == "persisted",
                    "idempotency result replays after repository restart");
        tests.check(repository.findById(userId).has_value(), "user survives repository restart");
        tests.check(repository.findAdminByUsername("admin").has_value() &&
                        repository.deviceCommand(commandNo).has_value() &&
                        repository.deviceCommand(commandNo)->status == "SUCCEEDED" &&
                        repository.charger(3)->status == ChargerStatus::Idle &&
                        repository.backup(backupNo).has_value() &&
                        repository.backup(backupNo)->verificationStatus == "SUCCEEDED" &&
                        repository.effectivePriceAdjustment(1, 1, 1788500000).has_value(),
                    "admin, command, backup and pricing state survive restart");
        const auto persistedBackup = repository.backup(backupNo);
        if (persistedBackup)
        {
            std::ofstream tamper(persistedBackup->storagePath, std::ios::binary | std::ios::app);
            tamper.put('\0');
            tamper.close();
            AdminOpsService adminOps(repository, repository, flows, numbers);
            const auto rejected = adminOps.verifyBackup(1, backupNo, now + std::chrono::seconds(4));
            tests.check(rejected.error == ncs::core::domain::ErrorCode::TransactionFailed &&
                            repository.backup(backupNo)->verificationStatus == "FAILED",
                        "tampered backup cannot pass checksum and isolated verification");
        }
        const auto active = flows.activeFlow(userId, now + std::chrono::seconds(12));
        tests.check(active.hasActiveFlow && active.flow->status == 40,
                    "charging flow survives repository restart");
        const auto progress = flows.progress(userId, flowNo, now + std::chrono::seconds(12));
        tests.check(progress.ok() && progress.value->durationSec == 600,
                    "billing resumes from the persisted start time");
        const auto settledEventsBefore = queryInteger(
            database.path(), "SELECT COUNT(*) FROM outbox_event WHERE event_type='order.settled'");
        executeSql(database.path(),
                   "CREATE TRIGGER fail_settlement_wallet BEFORE UPDATE ON wallet_account "
                   "BEGIN SELECT RAISE(ABORT,'injected settlement failure'); END");
        const auto failedSettlement = flows.settle(userId, flowNo, active.flow->version,
                                                   "USER_STOPPED", now + std::chrono::seconds(62));
        const auto failedFlow = flows.activeFlow(userId, now + std::chrono::seconds(62));
        tests.check(failedSettlement.error == ncs::core::domain::ErrorCode::TransactionFailed &&
                        failedFlow.hasActiveFlow && failedFlow.flow->status == 80 &&
                        repository.wallet(userId).balanceCent == 10000,
                    "settlement failure rolls back money and persists recoverable "
                    "status 80");
        tests.check(queryInteger(database.path(),
                                 "SELECT COUNT(*) FROM outbox_event WHERE "
                                 "event_type='order.settled'") == settledEventsBefore,
                    "failed settlement does not leave a successful notification event");
        executeSql(database.path(), "DROP TRIGGER fail_settlement_wallet");
        const auto settled = flows.settle(userId, flowNo, active.flow->version, "USER_STOPPED",
                                          now + std::chrono::seconds(62));
        tests.check(settled.ok() && settled.value->orderNo == orderNo,
                    "status 80 settlement retries with the pre-failure version in "
                    "one transaction");
        tests.check(queryInteger(database.path(),
                                 "SELECT COUNT(*) FROM outbox_event WHERE "
                                 "event_type='order.settled'") == settledEventsBefore + 1,
                    "successful settlement commits one reliable notification event");
    }

    {
        SqliteRepository repository(database.path());
        BusinessNumbers numbers(&repository);
        ChargeFlowService flows(repository, repository, repository, numbers, 60);
        AdminOpsService adminOps(repository, repository, flows, numbers);
        tests.check(!flows.activeFlow(userId, now).hasActiveFlow,
                    "settled flow remains terminal after another restart");
        tests.check(!repository.stationHasActiveFlow(1),
                    "station active-flow probe clears after settlement");
        tests.check(flows.receipt(userId, orderNo).ok(),
                    "settlement receipt remains available after restart");

        // UC-M-04: overdue tasks reach TIMED_OUT so the running-task dedup cannot
        // block the task type forever.
        MlTask agedTask;
        agedTask.taskNo = "MLAGED0000002";
        agedTask.taskType = "PREDICT";
        agedTask.status = "RUNNING";
        agedTask.createdAt = 1788499700;
        repository.addMlTask(agedTask);
        adminOps.completeTimedOutMlTasks(
            std::chrono::system_clock::time_point(std::chrono::seconds(1788501000)));
        const auto timedOut = repository.mlTask("MLAGED0000002");
        tests.check(timedOut && timedOut->status == "TIMED_OUT" &&
                        timedOut->finishedAt == 1788501000 && !repository.runningMlTask("PREDICT"),
                    "overdue ml task times out and frees the task type");

        // NFR-R-03 retention: newest per day for seven days, newest per week for
        // four weeks, failed diagnostics older than a week are removed.
        constexpr std::int64_t day = 24 * 3600;
        const std::vector<std::tuple<const char*, const char*, std::int64_t>> retentionFixtures{
            {"BKRETD30A", "SUCCEEDED", 1788500000 - 30 * day + 100},
            {"BKRETD30B", "SUCCEEDED", 1788500000 - 30 * day + 50},
            {"BKRETD30C", "SUCCEEDED", 1788500000 - 30 * day},
            {"BKRETD08", "SUCCEEDED", 1788500000 - 8 * day},
            {"BKRETD02", "SUCCEEDED", 1788500000 - 2 * day},
            {"BKRETF10", "FAILED", 1788500000 - 10 * day},
            {"BKRETF00", "FAILED", 1788500000},
        };
        for (const auto& [backupNo, status, createdAt] : retentionFixtures)
        {
            BackupRecord record;
            record.backupNo = backupNo;
            record.status = status;
            record.createdAt = createdAt;
            repository.addBackup(record);
        }
        executeSql(database.path(),
                   "INSERT INTO outbox_event(event_type,aggregate_type,aggregate_id,"
                   "from_status,to_status,reason_code,created_at,delivery_status,"
                   "delivery_attempts,available_at,published_at) VALUES"
                   "('test','test','old-delivered',0,0,'RETENTION',1,1,0,1,1),"
                   "('test','test','old-dead',0,0,'RETENTION',1,2,10,1,NULL),"
                   "('test','test','pending',0,0,'RETENTION',1,0,0,1,NULL),"
                   "('test','test','recent-delivered',0,0,'RETENTION',1788500000,"
                   "1,0,1788500000,1788500000)");
        repository.cleanupAdminRecords(1788500000 + 3600);
        tests.check(repository.backup("BKRETD30A").has_value() &&
                        !repository.backup("BKRETD30B").has_value() &&
                        !repository.backup("BKRETD30C").has_value() &&
                        repository.backup("BKRETD08").has_value() &&
                        repository.backup("BKRETD02").has_value() &&
                        !repository.backup("BKRETF10").has_value() &&
                        repository.backup("BKRETF00").has_value() &&
                        repository.backup(backupNo).has_value(),
                    "backup retention keeps seven daily and four weekly copies");
        tests.check(queryInteger(database.path(), "SELECT COUNT(*) FROM outbox_event WHERE "
                                                  "reason_code='RETENTION' AND aggregate_id IN "
                                                  "('old-delivered','old-dead')") == 0 &&
                        queryInteger(database.path(), "SELECT COUNT(*) FROM outbox_event WHERE "
                                                      "reason_code='RETENTION' AND aggregate_id IN "
                                                      "('pending','recent-delivered')") == 2,
                    "outbox cleanup prunes aged terminal rows but retains pending and recent rows");
        executeSql(database.path(), "DELETE FROM outbox_event WHERE reason_code='RETENTION'");

        repository.refreshHourlyMetrics(1788500000 - 7200, 1788500000);
        tests.check(
            queryInteger(database.path(), "SELECT MAX(bucket_at) FROM station_hourly_metric") <
                (1788500000 / 3600) * 3600,
            "hourly features exclude the current partial UTC hour");

        repository.ensureDevelopmentAdmin(false);
        tests.check(repository.findAdminByUsername("admin")->status == 0,
                    "non-development startup disables persisted demo credentials");
    }

    {
        SqliteRepository repository(database.path());
        const std::int64_t t0 = 1788500000;

        // Drain outbox rows left pending by the earlier blocks so this block
        // starts from a clean slate.
        const auto stale = repository.pollOutbox(t0 + 86400, 100000);
        std::vector<std::int64_t> staleIds;
        for (const auto& row : stale)
            staleIds.push_back(row.id);
        repository.markOutboxDead(staleIds);

        ChargerStatusEvent statusEvent;
        statusEvent.chargerId = 1;
        statusEvent.stationId = 1;
        statusEvent.fromStatus = 0;
        statusEvent.toStatus = 1;
        statusEvent.reason = "ALLOCATED";
        statusEvent.at = t0;
        repository.addChargerStatusEvent(statusEvent);
        const auto rows = repository.pollOutbox(t0, 10);
        tests.check(rows.size() == 1 && rows.front().eventType == "charger.statusChanged" &&
                        rows.front().aggregateType == "charger" &&
                        rows.front().aggregateId == "1" && rows.front().fromStatus == 0 &&
                        rows.front().toStatus == 1 && rows.front().reasonCode == "ALLOCATED" &&
                        rows.front().createdAt == t0 && rows.front().deliveryStatus == 0,
                    "charger status events land in the outbox with the charger aggregate");

        FlowEvent flowEvent;
        flowEvent.flowNo = flowNo; // the settled flow from the earlier block
        flowEvent.fromStatus = 10;
        flowEvent.toStatus = 20;
        flowEvent.reasonCode = "CREATED";
        flowEvent.at = t0 + 5;
        repository.addFlowEvent(flowEvent);
        const auto due = repository.pollOutbox(t0 + 5, 10);
        tests.check(due.size() == 2 && due.front().id < due.back().id,
                    "poll returns pending rows in id order including the flow event");

        FlowEvent futureEvent = flowEvent;
        futureEvent.at = t0 + 100;
        repository.addFlowEvent(futureEvent);
        tests.check(repository.pollOutbox(t0 + 5, 10).size() == 2,
                    "rows with a future available_at are not polled yet");
        tests.check(repository.pollOutbox(t0 + 100, 10).size() == 3,
                    "rows become available once available_at passes");
        tests.check(repository.pollOutbox(t0 + 100, 2).size() == 2, "poll respects the limit");

        const auto first = repository.pollOutbox(t0 + 100, 10);
        tests.check(first.size() == 3, "pending rows remain pollable before delivery");
        std::vector<std::int64_t> deliveredIds{first[0].id, first[1].id};
        repository.markOutboxDelivered(deliveredIds);
        const auto remaining = repository.pollOutbox(t0 + 100, 10);
        tests.check(remaining.size() == 1 && remaining.front().id == first[2].id,
                    "delivered rows leave the pending set");
        tests.check(queryInteger(database.path(),
                                 "SELECT COUNT(*) FROM outbox_event WHERE delivery_status=1 "
                                 "AND published_at IS NOT NULL") == 2,
                    "delivered rows carry status 1 and a published timestamp");

        repository.markOutboxAttempted({remaining.front().id});
        for (int attempt = 0; attempt < 9; ++attempt)
        {
            repository.markOutboxAttempted({remaining.front().id});
        }
        const auto none = repository.pollOutbox(t0 + 100, 10);
        const std::string attemptedSql = "SELECT delivery_status FROM outbox_event WHERE id=" +
                                         std::to_string(remaining.front().id);
        tests.check(none.empty() && queryInteger(database.path(), attemptedSql.c_str()) == 2,
                    "ten delivery attempts flip a row to dead");

        ChargerStatusEvent deadEvent = statusEvent;
        deadEvent.chargerId = 2;
        repository.addChargerStatusEvent(deadEvent);
        const auto deadRows = repository.pollOutbox(t0 + 100, 10);
        repository.markOutboxDead({deadRows.front().id});
        const std::string deadSql = "SELECT delivery_status FROM outbox_event WHERE id=" +
                                    std::to_string(deadRows.front().id);
        tests.check(repository.pollOutbox(t0 + 100, 10).empty() &&
                        queryInteger(database.path(), deadSql.c_str()) == 2,
                    "an explicitly dead row stops being polled");
    }

    return tests.result();
}
