#include "core/application/business_numbers.h"
#include "core/application/charge_flow_service.h"
#include "core/application/idempotency_service.h"
#include "core/application/wallet_service.h"
#include "infrastructure/sqlite/sqlite_repository.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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
                ("ncs-sqlite-migration-" + std::to_string(processId()) + ".db"))
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

std::int64_t queryInteger(const std::string& path, const std::string& sql)
{
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    {
        throw std::runtime_error("test database open failed");
    }
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
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
    const auto now = std::chrono::system_clock::time_point(std::chrono::seconds(1788500000));

    {
        // UC-D-02: reopening an older database re-applies the missing
        // migrations in order and preserves committed business data.
        TemporaryDatabase upgradeDatabase;
        std::int64_t upgradeUserId = 0;
        std::int64_t upgradeBalance = 0;
        {
            SqliteRepository repository(upgradeDatabase.path());
            repository.ensureDevelopmentAdmin(true);
            UserAccount account;
            account.username = "upgrade_user";
            account.phone = "13800137777";
            account.nickname = "升级用户";
            account.registeredAt = 1788500000;
            tests.check(repository.create(account) == AccountWriteResult::Success,
                        "upgrade database seeds a user and wallet");
            upgradeUserId = account.id;
            BusinessNumbers numbers(&repository);
            WalletService wallet(repository, repository, numbers);
            tests.check(wallet.recharge(upgradeUserId, 7000, now).ok(),
                        "upgrade database funds the wallet");
            upgradeBalance = repository.wallet(upgradeUserId).balanceCent;
        }
        // Roll the physical schema back to the v5 layout while keeping the
        // v1-v5 version rows, so the next open must run v6, v7 and v8. The
        // v8 seed rows are residue of this rollback, and the re-run's exact
        // identity check would flag them as conflicts — remove them with
        // test-local SQL so the fixture is an honest pre-v8 database
        // (production has no cleanup path; it aborts and asks for a backup).
        executeSql(upgradeDatabase.path(), "DELETE FROM schema_version WHERE version>=6");
        executeSql(upgradeDatabase.path(), "DROP INDEX IF EXISTS ux_ml_task_one_running_type");
        executeSql(upgradeDatabase.path(), "DROP TABLE IF EXISTS load_prediction");
        executeSql(upgradeDatabase.path(), "DROP TABLE IF EXISTS model_version");
        executeSql(upgradeDatabase.path(), "DROP TABLE IF EXISTS station_hourly_metric");
        executeSql(upgradeDatabase.path(), "DROP TABLE IF EXISTS dashboard_state");
        executeSql(upgradeDatabase.path(), "DROP INDEX IF EXISTS ix_order_status_settled");
        executeSql(upgradeDatabase.path(), "DROP INDEX IF EXISTS ix_order_status_started");
        executeSql(upgradeDatabase.path(),
                   "DELETE FROM flow_event WHERE flow_no IN (SELECT flow_no FROM charging_flow "
                   "WHERE user_id IN (SELECT id FROM user_account WHERE username LIKE "
                   "'sim_owner_%'));"
                   "DELETE FROM charging_order WHERE user_id IN (SELECT id FROM user_account "
                   "WHERE username LIKE 'sim_owner_%');"
                   "DELETE FROM charging_flow WHERE user_id IN (SELECT id FROM user_account "
                   "WHERE username LIKE 'sim_owner_%');"
                   "DELETE FROM wallet_transaction WHERE user_id IN (SELECT id FROM user_account "
                   "WHERE username LIKE 'sim_owner_%');"
                   "DELETE FROM recharge_order WHERE user_id IN (SELECT id FROM user_account "
                   "WHERE username LIKE 'sim_owner_%');"
                   "DELETE FROM wallet_account WHERE user_id IN (SELECT id FROM user_account "
                   "WHERE username LIKE 'sim_owner_%');"
                   "DELETE FROM user_account WHERE username LIKE 'sim_owner_%';"
                   "DELETE FROM charger WHERE station_id IN (SELECT id FROM station WHERE code IN "
                   "('CYGY','BJN','SJS','TZYH')) OR code IN ('ZGC-DC-04','ZGC-DC-05','ZGC-DC-06',"
                   "'ZGC-AC-03','ZGC-AC-04');"
                   "DELETE FROM station WHERE code IN ('CYGY','BJN','SJS','TZYH');"
                   "DELETE FROM region_tariff WHERE adcode IN ('110106','110107','110112');");
        {
            SqliteRepository repository(upgradeDatabase.path());
            tests.check(queryInteger(upgradeDatabase.path(),
                                     "SELECT MAX(version) FROM schema_version") == 8 &&
                            queryInteger(upgradeDatabase.path(),
                                         "SELECT COUNT(*) FROM dashboard_state") == 1 &&
                            repository.findById(upgradeUserId).has_value() &&
                            repository.wallet(upgradeUserId).balanceCent == upgradeBalance &&
                            repository.findAdminByUsername("admin").has_value(),
                        "reopening a v5 database re-applies v6, v7 and v8 in order and keeps "
                        "committed data");
        }
    }

    {
        // UC-D-03 / database design §8: concurrent allocators cannot share a
        // live flow, lose wallet updates, or double-grant idempotency leases.
        TemporaryDatabase database;
        SqliteRepository repository(database.path());
        BusinessNumbers numbers(&repository);
        WalletService wallet(repository, repository, numbers);
        ChargeFlowService flows(repository, repository, repository, numbers, 60);

        std::vector<UserAccount> racers(3);
        for (int i = 0; i < 3; ++i)
        {
            racers[i].username = "race_user_" + std::to_string(i);
            racers[i].phone = "1390011000" + std::to_string(i);
            racers[i].nickname = "并发用户" + std::to_string(i);
            racers[i].registeredAt = 1788500000;
            tests.check(repository.create(racers[i]) == AccountWriteResult::Success,
                        "concurrency racer account created");
            tests.check(wallet.recharge(racers[i].id, 10000, now).ok(), "concurrency racer funded");
        }

        std::atomic<int> userFlowWins{0};
        const auto raceForUser = [&](std::int64_t chargerId, int chargerType)
        {
            const auto result = flows.createFlow(racers[2].id, 1, chargerType, chargerId, now);
            if (result.ok())
                ++userFlowWins;
        };
        std::thread userThreadA(raceForUser, 3, 1);
        std::thread userThreadB(raceForUser, 5, 0);
        userThreadA.join();
        userThreadB.join();
        tests.check(
            userFlowWins.load() == 1 &&
                queryInteger(database.path(), "SELECT COUNT(*) FROM charging_flow WHERE user_id=" +
                                                  std::to_string(racers[2].id) +
                                                  " AND status IN (10,20,30,40,50,80)") == 1,
            "concurrent flows for one user leave exactly one live flow");

        std::atomic<int> rechargeWins{0};
        const auto raceRecharge = [&]
        {
            if (wallet.recharge(racers[0].id, 500, now + std::chrono::seconds(1)).ok())
                ++rechargeWins;
        };
        std::thread walletThreadA(raceRecharge);
        std::thread walletThreadB(raceRecharge);
        walletThreadA.join();
        walletThreadB.join();
        tests.check(rechargeWins.load() == 2 &&
                        repository.wallet(racers[0].id).balanceCent == 11000,
                    "concurrent recharges serialize without lost updates");

        IdempotencyService concurrentIdempotency(&repository);
        std::atomic<int> proceedWins{0};
        const auto raceBegin = [&]
        {
            const auto reservation = concurrentIdempotency.begin(
                "race:recharge", "2cb640c6-6995-4be5-9161-f0e2c1210077", "{\"amountCent\":500}",
                now + std::chrono::seconds(2), true);
            if (reservation.decision == IdempotencyDecision::Proceed)
                ++proceedWins;
            else if (reservation.leaseToken)
                concurrentIdempotency.abort("race:recharge", "2cb640c6-6995-4be5-9161-f0e2c1210077",
                                            *reservation.leaseToken);
        };
        std::thread leaseThreadA(raceBegin);
        std::thread leaseThreadB(raceBegin);
        leaseThreadA.join();
        leaseThreadB.join();
        tests.check(proceedWins.load() == 1,
                    "concurrent idempotency begins grant exactly one lease");
    }

    return tests.result();
}
