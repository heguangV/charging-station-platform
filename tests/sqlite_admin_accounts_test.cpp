#include "core/application/admin_repository.h"
#include "infrastructure/sqlite/sqlite_repository.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
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
                ("ncs-admin-accounts-" + std::to_string(processId()) + ".db"))
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
    TemporaryDatabase database;
    const std::int64_t at = 1788500000;
    constexpr std::string_view initialHash = "pbkdf2-admin-initial";
    constexpr std::string_view newHash = "pbkdf2-admin-changed";
    constexpr std::string_view reason = "新入职运营专员";

    {
        SqliteRepository repository(database.path());
        repository.ensureDevelopmentAdmin(true);
        tests.check(repository.check().ready() &&
                        repository.findAdminByUsername("admin").has_value(),
                    "fresh database is migrated with the development admin");

        const auto seeded = repository.findAdminByUsername("admin");
        const std::int64_t seedId = seeded->id;
        tests.check(seedId >= 1 && seeded->status == 1 && seeded->version == 1 &&
                        !seeded->mustChangePassword && repository.adminAccounts({}).total == 1,
                    "seeded admin account is enabled with default fields");

        AdminAccount created;
        tests.check(repository.createAdminAccount(seedId, "ops_wang", initialHash, reason, at,
                                                  created) == AdminAccountWriteResult::Success &&
                        created.id == seedId + 1 && created.status == 1 &&
                        created.mustChangePassword && created.version == 1 &&
                        created.roles.size() == 1 && created.roles.front() == Role::Operator,
                    "created account is a fresh OPERATOR flagged for a password change");
        tests.check(
            queryInteger(database.path(),
                         "SELECT COUNT(*) FROM admin_account WHERE username='ops_wang'") == 1 &&
                queryInteger(database.path(), "SELECT COUNT(*) FROM admin_role WHERE admin_id=" +
                                                  std::to_string(created.id)) == 1,
            "account and OPERATOR role rows are persisted");
        tests.check(
            repository.createAdminAccount(seedId, "ops_wang", initialHash, reason, at, created) ==
                AdminAccountWriteResult::UsernameExists,
            "duplicate account names surface as UsernameExists, not a crash");

        AdminAccountWriteResult outcome = AdminAccountWriteResult::NotFound;
        repository.withTransaction(
            [&]
            {
                outcome = repository.updateAdminAccountStatus(seedId, created.id, 0, "该管理员离岗",
                                                              99, at, created);
            });
        tests.check(outcome == AdminAccountWriteResult::VersionConflict,
                    "a stale version cannot disable the account");
        AdminAccount disabled;
        repository.withTransaction(
            [&]
            {
                outcome = repository.updateAdminAccountStatus(seedId, created.id, 0, "该管理员离岗",
                                                              1, at, disabled);
            });
        tests.check(outcome == AdminAccountWriteResult::Success && disabled.status == 0 &&
                        disabled.version == 2,
                    "disabling bumps the version and writes the audit row");
        tests.check(queryInteger(database.path(),
                                 "SELECT COUNT(*) FROM ops_log WHERE action='ADMIN_DISABLED'") == 1,
                    "the disable audit row is stored in SQL");
        tests.check(
            repository.updateAdminAccountStatus(seedId, 99999, 0, "无此账号", 1, at, disabled) ==
                AdminAccountWriteResult::NotFound,
            "status change for a missing account reports NotFound");

        AdminAccount changed;
        tests.check(repository.changeAdminAccountPassword(created.id, created.id, initialHash,
                                                          newHash, at, changed) ==
                            AdminAccountWriteResult::Success &&
                        !changed.mustChangePassword && changed.passwordHash == newHash &&
                        changed.version == 3,
                    "changing the own password clears the flag and bumps the version");
        tests.check(repository.changeAdminAccountPassword(created.id, created.id, initialHash,
                                                          "another-hash", at, changed) ==
                        AdminAccountWriteResult::HashMismatch,
                    "a stale credential digest loses the password compare-and-swap");
        tests.check(repository.changeAdminAccountPassword(created.id, 99999, initialHash, newHash,
                                                          at, changed) ==
                        AdminAccountWriteResult::NotFound,
                    "password change for a missing account reports NotFound");

        AdminAccount enabled;
        tests.check(repository.updateAdminAccountStatus(seedId, created.id, 1, "离岗原因解除", 3,
                                                        at, enabled) ==
                            AdminAccountWriteResult::Success &&
                        enabled.status == 1 && enabled.version == 4,
                    "enabling restores the account with the unchanged password");

        AdminAccountQuery query;
        query.page = 1;
        query.pageSize = 20;
        const auto listing = repository.adminAccounts(query);
        tests.check(listing.total == 2 && listing.items.size() == 2 &&
                        listing.items[0].username == "ops_wang" &&
                        listing.items[1].username == "admin",
                    "admin accounts list newest first");
    }

    // Reopen: every mutation must survive on disk with its audit trail.
    {
        SqliteRepository repository(database.path());
        const auto reloaded = repository.findAdminById(2);
        tests.check(reloaded.has_value() && reloaded->username == "ops_wang" &&
                        reloaded->status == 1 && !reloaded->mustChangePassword &&
                        reloaded->passwordHash == newHash && reloaded->version == 4,
                    "account state survives a repository reopen");

        AuditEventQuery audit;
        audit.pageSize = 50;
        audit.action = "ADMIN_CREATED";
        const auto createdEvents = repository.auditEvents(audit);
        audit.action = "ADMIN_DISABLED";
        const auto disabledEvents = repository.auditEvents(audit);
        audit.action = "ADMIN_ENABLED";
        const auto enabledEvents = repository.auditEvents(audit);
        audit.action = "ADMIN_PASSWORD_CHANGED";
        const auto changedEvents = repository.auditEvents(audit);
        tests.check(createdEvents.size() == 1 && createdEvents[0].targetId == "2" &&
                        disabledEvents.size() == 1 && enabledEvents.size() == 1 &&
                        changedEvents.size() == 1 && changedEvents[0].actorAdminId == 2,
                    "audit trail persists create, status and password events");

        const auto seeded = repository.findAdminByUsername("admin");
        const auto listing = repository.adminAccounts({1, 20});
        AdminAccount ignored;
        tests.check(listing.total == 2 && repository.changeAdminAccountPassword(
                                              seeded->id, seeded->id, initialHash, newHash, at,
                                              ignored) == AdminAccountWriteResult::HashMismatch,
                    "demo admin password remains untouched by account tests");
    }

    // Rollback: an exception inside the transaction leaves no partial row.
    {
        SqliteRepository repository(database.path());
        bool threw = false;
        try
        {
            repository.withTransaction(
                [&]
                {
                    AdminAccount created;
                    const auto write = repository.createAdminAccount(1, "ops_rollback", initialHash,
                                                                     reason, at, created);
                    if (write != AdminAccountWriteResult::Success)
                        throw std::runtime_error("create failed");
                    throw std::runtime_error("simulated failure after the insert");
                });
        }
        catch (const std::runtime_error&)
        {
            threw = true;
        }
        tests.check(threw && queryInteger(database.path(), "SELECT COUNT(*) FROM admin_account "
                                                           "WHERE username='ops_rollback'") == 0,
                    "a failing transaction rolls back the account insert");
    }

    // UC-A-09 first-OWNER bootstrap: the demo OWNER and the existing OPERATOR
    // (ops_wang) never block it; only a real OWNER makes it one-shot.
    {
        SqliteRepository repository(database.path());
        const std::int64_t countBefore =
            queryInteger(database.path(), "SELECT COUNT(*) FROM admin_account");

        bool threw = false;
        try
        {
            repository.bootstrapOwnerAccount("ops_wang", initialHash, at);
        }
        catch (const std::runtime_error&)
        {
            threw = true;
        }
        tests.check(threw && queryInteger(database.path(), "SELECT COUNT(*) FROM admin_account") ==
                                 countBefore,
                    "bootstrapping an existing username throws and rolls back");

        const auto bootstrapped = repository.bootstrapOwnerAccount("root_owner", initialHash, at);
        tests.check(bootstrapped.has_value() && bootstrapped->username == "root_owner" &&
                        bootstrapped->status == 1 && bootstrapped->mustChangePassword &&
                        bootstrapped->version == 1 && bootstrapped->roles.size() == 1 &&
                        bootstrapped->roles.front() == Role::Owner,
                    "bootstrap creates an enabled OWNER flagged for a password change");
        tests.check(queryInteger(database.path(), "SELECT is_demo FROM admin_account WHERE "
                                                  "username='root_owner'") == 0 &&
                        queryInteger(database.path(), "SELECT COUNT(*) FROM admin_role WHERE "
                                                      "admin_id=" +
                                                          std::to_string(bootstrapped->id) +
                                                          " AND role='OWNER'") == 1 &&
                        queryInteger(database.path(), "SELECT COUNT(*) FROM ops_log WHERE "
                                                      "action='ADMIN_CREATED' AND reason="
                                                      "'bootstrap-owner'") == 1,
                    "bootstrap persists the non-demo OWNER role and its audit row");

        const auto opsAccount = repository.findAdminByUsername("ops_wang");
        tests.check(opsAccount.has_value() &&
                        queryInteger(database.path(), "SELECT COUNT(*) FROM admin_role WHERE "
                                                      "admin_id=" +
                                                          std::to_string(opsAccount->id) +
                                                          " AND role='OPERATOR'") == 1 &&
                        queryInteger(database.path(), "SELECT COUNT(*) FROM admin_role WHERE "
                                                      "admin_id=" +
                                                          std::to_string(opsAccount->id) +
                                                          " AND role='OWNER'") == 0,
                    "an OPERATOR created by the account service is untouched and does not block "
                    "the bootstrap");

        tests.check(!repository.bootstrapOwnerAccount("root_owner_2", initialHash, at).has_value(),
                    "a second bootstrap is refused once a non-demo OWNER exists");
        tests.check(queryInteger(database.path(), "SELECT COUNT(*) FROM admin_account") ==
                            countBefore + 1 &&
                        queryInteger(database.path(), "SELECT COUNT(*) FROM ops_log WHERE "
                                                      "action='ADMIN_CREATED' AND reason="
                                                      "'bootstrap-owner'") == 1,
                    "the refused bootstrap leaves accounts and the audit trail unchanged");
    }

    return tests.result();
}
