#include "infrastructure/postgres/postgres_repository.h"
#include "infrastructure/postgres/postgres_row_mappers.h"

#include "core/application/security_crypto.h"

namespace ncs::infrastructure::postgres
{

using namespace ncs::core::application;
using namespace detail;

AdminAccountPage PostgresRepository::adminAccounts(const AdminAccountQuery& query)
{
    return useDatabase(
        this, config_,
        [&](QSqlDatabase* database)
        {
            Statement count(database, "SELECT COUNT(*) FROM admin_account");
            AdminAccountPage page;
            page.page = query.page;
            page.pageSize = query.pageSize;
            if (count.row())
                page.total = static_cast<int>(count.integer(0));
            Statement select(database,
                             "SELECT id,username,password_hash,status,must_change_password,version "
                             "FROM admin_account ORDER BY id DESC LIMIT ? OFFSET ?");
            select.bind(1, query.pageSize);
            select.bind(2, static_cast<std::int64_t>(query.page - 1) * query.pageSize);
            while (select.row())
                page.items.push_back(readAdmin(database, select));
            return page;
        });
}

AdminAccountWriteResult PostgresRepository::createAdminAccount(const std::int64_t actorAdminId,
                                                               const std::string_view username,
                                                               const std::string_view passwordHash,
                                                               const std::string_view reason,
                                                               const std::int64_t at,
                                                               AdminAccount& created)
{
    try
    {
        withTransaction(
            [&]
            {
                Statement insert(transactionContext.database,
                                 "INSERT INTO admin_account(username,password_hash,status,"
                                 "must_change_password,is_demo,version) VALUES(?,?,1,1,0,1) "
                                 "RETURNING id");
                insert.bind(1, username);
                insert.bind(2, passwordHash);
                if (!insert.row())
                    throw std::runtime_error("admin insert did not return an id");
                created.id = insert.integer(0);
                created.username = std::string(username);
                created.passwordHash = std::string(passwordHash);
                created.status = 1;
                created.roles = {Role::Operator};
                created.mustChangePassword = true;
                created.version = 1;
                Statement role(transactionContext.database,
                               "INSERT INTO admin_role(admin_id,role) VALUES(?,?)");
                role.bind(1, created.id);
                role.bind(2, "OPERATOR");
                role.execute();
                addAuditEvent(AuditEvent{actorAdminId, "ADMIN_CREATED", "ADMIN",
                                         std::to_string(created.id), std::string(reason), at});
            });
        return AdminAccountWriteResult::Success;
    }
    catch (const std::exception&)
    {
        if (findAdminByUsername(username))
            return AdminAccountWriteResult::UsernameExists;
        throw;
    }
}

// 一次性 OWNER 引导（--bootstrap-owner 离线模式）：已存在任何非演示 OWNER
// 则返回 nullopt（one-shot）；演示 OWNER 不阻塞引导。用户名被占用直接抛错。
std::optional<AdminAccount> PostgresRepository::bootstrapOwnerAccount(
    const std::string_view username, const std::string_view passwordHash, const std::int64_t at)
{
    AdminAccount created;
    withTransaction(
        [&]
        {
            advisoryTransactionLock(transactionContext.database, "bootstrap-owner");
            // A demo OWNER (is_demo=1, disabled in production) never blocks the
            // bootstrap; only a real, non-demo OWNER makes it one-shot.
            Statement existing(transactionContext.database,
                               "SELECT 1 FROM admin_account a JOIN admin_role r ON "
                               "r.admin_id=a.id WHERE r.role='OWNER' AND a.is_demo=0");
            if (existing.row())
            {
                created.id = 0;
                return;
            }
            Statement usernameTaken(transactionContext.database,
                                    "SELECT 1 FROM admin_account WHERE username=?");
            usernameTaken.bind(1, username);
            if (usernameTaken.row())
                throw std::runtime_error("bootstrap owner username already exists");
            Statement insert(
                transactionContext.database,
                "INSERT INTO admin_account(username,password_hash,status,"
                "must_change_password,is_demo,version) VALUES(?,?,1,1,0,1) RETURNING id");
            insert.bind(1, username);
            insert.bind(2, passwordHash);
            if (!insert.row())
                throw std::runtime_error("owner insert did not return an id");
            created.id = insert.integer(0);
            created.username = std::string(username);
            created.passwordHash = std::string(passwordHash);
            created.status = 1;
            created.roles = {Role::Owner};
            created.mustChangePassword = true;
            created.version = 1;
            Statement role(transactionContext.database,
                           "INSERT INTO admin_role(admin_id,role) VALUES(?,?)");
            role.bind(1, created.id);
            role.bind(2, "OWNER");
            role.execute();
            addAuditEvent(AuditEvent{created.id, "ADMIN_CREATED", "ADMIN",
                                     std::to_string(created.id), "bootstrap-owner", at});
        });
    return created.id == 0 ? std::nullopt : std::optional<AdminAccount>(std::move(created));
}

AdminAccountWriteResult PostgresRepository::updateAdminAccountStatus(
    const std::int64_t actorAdminId, const std::int64_t adminId, const int status,
    const std::string_view reason, const std::int64_t expectedVersion, const std::int64_t at,
    AdminAccount& updated)
{
    AdminAccountWriteResult result = AdminAccountWriteResult::NotFound;
    withTransaction(
        [&]
        {
            Statement update(transactionContext.database,
                             "UPDATE admin_account SET status=?,version=version+1 "
                             "WHERE id=? AND version=?");
            update.bind(1, status);
            update.bind(2, adminId);
            update.bind(3, expectedVersion);
            update.execute();
            if (update.rowsAffected() == 0)
            {
                const auto current = findAdminById(adminId);
                result = current ? AdminAccountWriteResult::VersionConflict
                                 : AdminAccountWriteResult::NotFound;
                return;
            }
            updated = *findAdminById(adminId);
            addAuditEvent(AuditEvent{actorAdminId, status == 0 ? "ADMIN_DISABLED" : "ADMIN_ENABLED",
                                     "ADMIN", std::to_string(adminId), std::string(reason), at});
            result = AdminAccountWriteResult::Success;
        });
    return result;
}

AdminAccountWriteResult PostgresRepository::changeAdminAccountPassword(
    const std::int64_t actorAdminId, const std::int64_t adminId,
    const std::string_view expectedCurrentHash, const std::string_view newPasswordHash,
    const std::int64_t at, AdminAccount& updated)
{
    AdminAccountWriteResult result = AdminAccountWriteResult::NotFound;
    withTransaction(
        [&]
        {
            Statement update(transactionContext.database,
                             "UPDATE admin_account SET password_hash=?,must_change_password=0,"
                             "version=version+1 WHERE id=? AND password_hash=?");
            update.bind(1, newPasswordHash);
            update.bind(2, adminId);
            update.bind(3, expectedCurrentHash);
            update.execute();
            if (update.rowsAffected() == 0)
            {
                const auto current = findAdminById(adminId);
                result = !current ? AdminAccountWriteResult::NotFound
                                  : AdminAccountWriteResult::HashMismatch;
                return;
            }
            updated = *findAdminById(adminId);
            addAuditEvent(AuditEvent{
                actorAdminId, "ADMIN_PASSWORD_CHANGED", "ADMIN", std::to_string(adminId), {}, at});
            result = AdminAccountWriteResult::Success;
        });
    return result;
}

} // namespace ncs::infrastructure::postgres
