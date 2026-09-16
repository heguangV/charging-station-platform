#include "core/application/admin_ops_service.h"
#include "core/application/business_numbers.h"
#include "core/application/charge_flow_service.h"
#include "core/application/wallet_service.h"
#include "infrastructure/postgres/postgres_repository.h"
#include "infrastructure/postgres/postgres_repository_detail.h"

#include <QCoreApplication>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{
void checkRetention(ncs::infrastructure::postgres::PostgresRepository& repository,
                    const ncs::core::application::BackupRecord& prototype)
{
    using namespace ncs::infrastructure::postgres::detail;
    struct RollbackFixture
    {
    };
    constexpr std::int64_t day = 86400;
    constexpr std::int64_t now = (2000 * 7 + 6) * day + 3600;
    try
    {
        repository.withTransaction(
            [&]
            {
                // Isolate metadata and roll it back; never touch the real dump artifact.
                execute(transactionContext.database, "DELETE FROM backup_record");
                const auto add =
                    [&](const std::string& name, std::int64_t at, const std::string& status)
                {
                    auto record = prototype;
                    record.backupNo = name;
                    record.createdAt = at;
                    record.status = status;
                    record.storagePath.clear();
                    repository.addBackup(record);
                };
                for (int age = 0; age <= 8; ++age)
                {
                    add("daily" + std::to_string(age), now - age * day, "SUCCEEDED");
                    add("duplicate" + std::to_string(age), now - age * day - 1, "SUCCEEDED");
                }
                for (int age : {14, 21, 28})
                    add("weekly" + std::to_string(age), now - age * day, "SUCCEEDED");
                add("failed_boundary", now - 7 * day, "FAILED");
                add("failed_expired", now - 7 * day - 1, "FAILED");
                add("failed_recent", now, "FAILED");
                repository.cleanupAdminRecords(now);
                for (int age = 0; age <= 8; ++age)
                    if (repository.backup("daily" + std::to_string(age)).has_value() !=
                            (age <= 7) ||
                        repository.backup("duplicate" + std::to_string(age)))
                        throw std::runtime_error("daily/weekly retention union mismatch");
                if (!repository.backup("weekly14") || !repository.backup("weekly21") ||
                    repository.backup("weekly28") || !repository.backup("failed_boundary") ||
                    !repository.backup("failed_recent") || repository.backup("failed_expired"))
                    throw std::runtime_error("weekly limit or failed retention boundary mismatch");
                execute(transactionContext.database, "DELETE FROM backup_record");
                for (int week = 0; week < 8; ++week)
                    add("sparse" + std::to_string(week), now - week * 7 * day, "SUCCEEDED");
                repository.cleanupAdminRecords(now);
                for (int week = 0; week < 8; ++week)
                    if (repository.backup("sparse" + std::to_string(week)).has_value() !=
                        (week < 7))
                        throw std::runtime_error("sparse daily representatives were lost");
                throw RollbackFixture{};
            });
    }
    catch (const RollbackFixture&)
    {
    }
    if (!repository.backup(prototype.backupNo))
        throw std::runtime_error("retention fixture did not roll back");
}

// Synchronize on an actual PostgreSQL lock wait, not a scheduler-dependent sleep.
void awaitBlockedWriter()
{
    using namespace ncs::infrastructure::postgres::detail;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline)
    {
        Statement query(transactionContext.database,
                        "SELECT COUNT(*) FROM pg_locks waiter JOIN pg_locks holder "
                        "ON waiter.transactionid=holder.transactionid "
                        "WHERE NOT waiter.granted AND holder.granted "
                        "AND holder.pid=pg_backend_pid() AND waiter.pid<>holder.pid");
        if (query.row() && query.integer(0) > 0)
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("concurrent writer never reached the expected row lock");
}
} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    if (argc != 9)
    {
        std::cerr << "usage: postgres_repository_test <host> <port> <database> <user> "
                     "<migrations> <backups> <pg_dump> <pg_restore>\n";
        return 2;
    }
    ncs::infrastructure::postgres::PostgresConfig config;
    config.host = argv[1];
    config.port = static_cast<std::uint16_t>(std::stoi(argv[2]));
    config.database = argv[3];
    config.user = argv[4];
    config.sslMode = "disable";
    config.migrationsDirectory = argv[5];
    config.backupDirectory = argv[6];
    config.pgDumpExecutable = argv[7];
    config.pgRestoreExecutable = argv[8];
    try
    {
        using namespace ncs::core::application;
        ncs::infrastructure::postgres::PostgresRepository repository(config);
        {
            using namespace ncs::infrastructure::postgres::detail;
            Connection connection(config, &repository.connectionSlots());
            Statement literal(connection.get(), "SELECT ':p1', CAST(? AS BIGINT), '12:30'");
            literal.bind(1, 42);
            if (!literal.row() || literal.text(0) != ":p1" || literal.integer(1) != 42)
                throw std::runtime_error("PostgreSQL positional binding failed");
            connection.execute("CREATE TEMP TABLE redaction_fixture(value TEXT UNIQUE)");
            connection.execute("INSERT INTO redaction_fixture VALUES('private-business-value')");
            bool rejected = false;
            try
            {
                Statement duplicate(connection.get(), "INSERT INTO redaction_fixture VALUES(?)");
                duplicate.bind(1, std::string_view("private-business-value"));
                duplicate.execute();
            }
            catch (const std::runtime_error& error)
            {
                rejected = true;
                if (std::string(error.what()) != "database statement failed")
                    throw std::runtime_error("PostgreSQL DETAIL was not redacted");
            }
            if (!rejected)
                throw std::runtime_error("PostgreSQL duplicate unexpectedly accepted");
        }
        const auto readiness = repository.check();
        if (!readiness.ready())
        {
            std::cerr << "PostgreSQL repository is not ready after migration\n";
            return 1;
        }
        if (repository.stations().size() != 5 || repository.chargers({}, {}, {}).size() != 48)
        {
            std::cerr << "PostgreSQL demo fleet does not match UC-D-02\n";
            return 1;
        }
        repository.ensureDevelopmentAdmin(true);
        const auto admin = repository.findAdminByUsername("admin");
        if (!admin)
            throw std::runtime_error("development admin was not created");

        UserAccount account;
        account.username = "postgres_contract_user";
        account.phone = "13800138888";
        account.nickname = "PostgreSQL contract";
        account.registeredAt = 1788500000;
        if (repository.create(account) != AccountWriteResult::Success || account.id <= 300)
            throw std::runtime_error("user identity insert/RETURNING failed");

        AdminUserQuery userQuery;
        userQuery.phoneLast4 = "8888";
        if (repository.listManagedUsers(userQuery).total != 1)
            throw std::runtime_error("nullable admin user filters failed");

        const auto now = std::chrono::system_clock::time_point(std::chrono::seconds(1788500000));
        BusinessNumbers numbers(&repository);
        WalletService wallet(repository, repository, numbers);
        ChargeFlowService flows(repository, repository, repository, numbers, 60);
        if (!wallet.recharge(account.id, 10000, now).ok())
            throw std::runtime_error("wallet transaction failed");

        const auto beforeConcurrentRecharge = repository.wallet(account.id);
        std::atomic<int> rechargeFailures{0};
        std::vector<std::thread> rechargeWorkers;
        for (int index = 0; index < 8; ++index)
        {
            rechargeWorkers.emplace_back(
                [&]
                {
                    try
                    {
                        if (!wallet.recharge(account.id, 100, now + std::chrono::seconds(10)).ok())
                            ++rechargeFailures;
                    }
                    catch (...)
                    {
                        ++rechargeFailures;
                    }
                });
        }
        for (auto& worker : rechargeWorkers)
            worker.join();
        const auto afterConcurrentRecharge = repository.wallet(account.id);
        if (rechargeFailures != 0 ||
            afterConcurrentRecharge.balanceCent != beforeConcurrentRecharge.balanceCent + 800)
            throw std::runtime_error("concurrent wallet row locking lost an update");

        // Force recharge to wait on the user held by createFlow's transaction.
        // The old wallet-first order deadlocked here when createFlow read the wallet.
        std::future<bool> mixedRecharge;
        ServiceResult<FlowView> created;
        repository.withTransaction(
            [&]
            {
                (void)repository.findById(account.id);
                mixedRecharge = std::async(std::launch::async, [&]
                                           { return wallet.recharge(account.id, 100, now).ok(); });
                awaitBlockedWriter();
                created = flows.createFlow(account.id, 1, 1, std::nullopt, now);
            });
        if (!mixedRecharge.get())
            throw std::runtime_error("mixed flow/recharge transaction failed");
        if (!created.ok() || !created.value->quote)
            throw std::runtime_error("flow allocation failed");

        // Force deletion to observe a flow committed after it started waiting.
        // A plain SELECT followed by UPDATE used to anonymize this active account.
        UserAccount racingAccount;
        racingAccount.username = "postgres_delete_race";
        racingAccount.phone = "13800137777";
        racingAccount.registeredAt = 1788500000;
        if (repository.create(racingAccount) != AccountWriteResult::Success ||
            !wallet.recharge(racingAccount.id, 10000, now).ok())
            throw std::runtime_error("delete race setup failed");
        std::future<AccountWriteResult> deletion;
        repository.withTransaction(
            [&]
            {
                (void)repository.findById(racingAccount.id);
                deletion = std::async(std::launch::async,
                                      [&]
                                      {
                                          UserAccount removed;
                                          return repository.anonymize(racingAccount.id, removed);
                                      });
                awaitBlockedWriter();
                if (!flows.createFlow(racingAccount.id, 1, 1, std::nullopt, now).ok())
                    throw std::runtime_error("delete race flow creation failed");
            });
        if (deletion.get() != AccountWriteResult::ActiveFlowExists ||
            !repository.findById(racingAccount.id))
            throw std::runtime_error("anonymized an account with a concurrent active flow");
        const auto confirmed =
            flows.confirmQuote(account.id, created.value->flowNo, created.value->quote->quoteNo,
                               created.value->version, now + std::chrono::seconds(1));
        if (!confirmed.ok())
            throw std::runtime_error("quote confirmation failed");
        const auto started =
            flows.start(account.id, created.value->flowNo, confirmed.value->version, std::nullopt,
                        std::nullopt, now + std::chrono::seconds(2));
        if (!started.ok())
            throw std::runtime_error("flow start failed");
        const auto active = flows.activeFlow(account.id, now + std::chrono::seconds(3));
        if (!active.hasActiveFlow || !active.flow)
            throw std::runtime_error("active flow lookup failed");
        if (!flows
                 .settle(account.id, created.value->flowNo, active.flow->version, "USER_STOPPED",
                         now + std::chrono::seconds(62))
                 .ok())
            throw std::runtime_error("flow settlement failed");

        repository.refreshHourlyMetrics(1788492800, 1788500000);
        const auto metrics = repository.hourlyMetrics(1788492800, 1788500000, {}, {}, 100);
        if (metrics.items.empty())
            throw std::runtime_error("hourly metric refresh failed");

        repository.withReadTransaction(
            [&]
            {
                (void)repository.listAccounts();
                (void)repository.stations();
                (void)repository.chargers({}, {}, {});
                (void)repository.settledOrders(1785908000, 1788500000, std::nullopt);
                (void)repository.predictions(std::nullopt, 24, 1788500000);
            });

        AdminOpsService adminOps(repository, repository, flows, numbers);
        ServiceResult<BackupRecord> backup;
        repository.withTransaction([&] { backup = adminOps.createBackup(admin->id, now); });
        if (!backup.ok() || backup.value->sizeBytes <= 0 || backup.value->checksum.empty())
            throw std::runtime_error("pg_dump backup failed");
        const auto verified = adminOps.verifyBackup(admin->id, backup.value->backupNo, now);
        if (!verified.ok() || verified.value->verificationStatus != "SUCCEEDED")
            throw std::runtime_error("pg_restore archive verification failed");

        checkRetention(repository, *backup.value);

        // Expired failed artifacts age out; a failed file removal retains its row.
        BackupRecord expired = *backup.value;
        expired.backupNo = "expired_dump";
        expired.status = "FAILED";
        expired.createdAt = 1;
        expired.storagePath =
            (std::filesystem::path(config.backupDirectory) / "expired_dump.dump").string();
        std::filesystem::copy_file(backup.value->storagePath, expired.storagePath);
        repository.addBackup(expired);
        BackupRecord blocked = expired;
        blocked.backupNo = "blocked_dump";
        blocked.storagePath =
            (std::filesystem::path(config.backupDirectory) / "blocked_dump.dump").string();
        std::filesystem::create_directory(blocked.storagePath);
        std::ofstream(std::filesystem::path(blocked.storagePath) / "child") << "retained";
        repository.addBackup(blocked);
        repository.cleanupAdminRecords(1788500000);
        if (repository.backup(expired.backupNo) || std::filesystem::exists(expired.storagePath) ||
            !repository.backup(blocked.backupNo))
            throw std::runtime_error(
                "backup retention did not remove dump or retain failed deletion");
        std::filesystem::remove(std::filesystem::path(blocked.storagePath) / "child");
        repository.cleanupAdminRecords(1788500000);
        if (repository.backup(blocked.backupNo))
            throw std::runtime_error("backup cleanup retry failed");

        ncs::infrastructure::postgres::PostgresRepository reopened(config);
        if (!reopened.check().ready() || reopened.stations().size() != 5 ||
            !reopened.findById(account.id))
        {
            std::cerr << "PostgreSQL migration reopen is not idempotent\n";
            return 1;
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << "PostgreSQL repository test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
