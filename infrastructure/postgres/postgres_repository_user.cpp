#include "infrastructure/postgres/postgres_repository.h"
#include "infrastructure/postgres/postgres_row_mappers.h"

namespace ncs::infrastructure::postgres
{

using namespace ncs::core::application;
using namespace detail;

// ---- 用户账户域：查询/注册/资料/凭据/头像/注销 ----
// 注册在一个事务内写入 user_account + user_credential + wallet_account；
// 注销走匿名化（不可逆占位 + 删除凭据头像），不删主键以保历史订单引用。
std::optional<UserAccount> PostgresRepository::findById(const std::int64_t id) const
{
    return useDatabase(
        this, config_,
        [&](QSqlDatabase* database) -> std::optional<UserAccount>
        {
            const std::string sql = std::string(userSelect) + "WHERE u.id=? AND u.deleted=0" +
                                    (inWriteTransaction(this) ? " FOR UPDATE OF u" : "");
            Statement query(database, sql.c_str());
            query.bind(1, id);
            return query.row() ? std::optional<UserAccount>(readUser(query)) : std::nullopt;
        });
}

std::optional<UserAccount> PostgresRepository::findByPhone(const std::string_view phone) const
{
    return useDatabase(
        this, config_,
        [&](QSqlDatabase* database) -> std::optional<UserAccount>
        {
            Statement query(database,
                            (std::string(userSelect) + "WHERE u.phone=? AND u.deleted=0").c_str());
            query.bind(1, phone);
            return query.row() ? std::optional<UserAccount>(readUser(query)) : std::nullopt;
        });
}

std::optional<UserAccount>
PostgresRepository::findByLoginName(const std::string_view loginName) const
{
    return useDatabase(
        this, config_,
        [&](QSqlDatabase* database) -> std::optional<UserAccount>
        {
            Statement query(database, (std::string(userSelect) +
                                       "WHERE (u.username=? OR u.phone=?) AND u.deleted=0")
                                          .c_str());
            query.bind(1, loginName);
            query.bind(2, loginName);
            return query.row() ? std::optional<UserAccount>(readUser(query)) : std::nullopt;
        });
}

// 注册：同一事务写 user_account + user_credential + wallet_account；
// 失败后回查区分 UsernameExists/PhoneExists，无法归因则原样抛出。
AccountWriteResult PostgresRepository::create(UserAccount& account)
{
    try
    {
        withTransaction(
            [&]
            {
                Statement insert(transactionContext.database,
                                 "INSERT INTO "
                                 "user_account(username,phone,nickname,status,registered_"
                                 "at,balance_cent,debt_cent,has_active_flow,version,"
                                 "deleted) VALUES(?,?,?,?,?,?,?,?,?,0) RETURNING id");
                insert.bind(1, account.username);
                insert.bind(2, account.phone);
                insert.bind(3, account.nickname);
                insert.bind(4, account.status);
                insert.bind(5, account.registeredAt);
                insert.bind(6, account.balanceCent);
                insert.bind(7, account.debtCent);
                insert.bind(8, account.hasActiveFlow ? 1 : 0);
                insert.bind(9, account.version);
                if (!insert.row())
                    throw std::runtime_error("user insert did not return an id");
                account.id = insert.integer(0);
                if (account.passwordHash)
                {
                    Statement credential(
                        transactionContext.database,
                        "INSERT INTO user_credential(user_id,password_hash) VALUES(?,?)");
                    credential.bind(1, account.id);
                    credential.bind(2, *account.passwordHash);
                    credential.execute();
                }
                Statement walletInsert(transactionContext.database,
                                       "INSERT INTO "
                                       "wallet_account(user_id,balance_cent,debt_cent,"
                                       "version,updated_at) VALUES(?,?,?,?,?)");
                walletInsert.bind(1, account.id);
                walletInsert.bind(2, account.balanceCent);
                walletInsert.bind(3, account.debtCent);
                walletInsert.bind(4, 1);
                walletInsert.bind(5, account.registeredAt);
                walletInsert.execute();
            });
        return AccountWriteResult::Success;
    }
    catch (const std::exception&)
    {
        if (findByLoginName(account.username))
            return AccountWriteResult::UsernameExists;
        if (findByPhone(account.phone))
            return AccountWriteResult::PhoneExists;
        throw;
    }
}

AccountWriteResult PostgresRepository::updateNickname(const std::int64_t id,
                                                      const std::int64_t expectedVersion,
                                                      std::string nickname, UserAccount& updated)
{
    return useDatabase(this, config_,
                       [&](QSqlDatabase* database)
                       {
                           Statement update(database,
                                            "UPDATE user_account SET nickname=?,version=version+1 "
                                            "WHERE id=? AND version=? AND deleted=0");
                           update.bind(1, nickname);
                           update.bind(2, id);
                           update.bind(3, expectedVersion);
                           update.execute();
                           if (update.rowsAffected() == 0)
                               return findById(id) ? AccountWriteResult::VersionConflict
                                                   : AccountWriteResult::NotFound;
                           updated = *findById(id);
                           return AccountWriteResult::Success;
                       });
}

AccountWriteResult PostgresRepository::updateStatus(const std::int64_t id, const int status,
                                                    UserAccount& updated)
{
    if (status != 0 && status != 1)
        return AccountWriteResult::NotFound;
    return useDatabase(this, config_,
                       [&](QSqlDatabase* database)
                       {
                           Statement update(
                               database, "UPDATE user_account SET status=?,version=version+1 WHERE "
                                         "id=? AND deleted=0");
                           update.bind(1, status);
                           update.bind(2, id);
                           update.execute();
                           if (update.rowsAffected() == 0)
                               return AccountWriteResult::NotFound;
                           updated = *findById(id);
                           return AccountWriteResult::Success;
                       });
}

AccountWriteResult PostgresRepository::updateCredential(const std::int64_t id, std::string username,
                                                        std::string passwordHash,
                                                        UserAccount& updated)
{
    try
    {
        withTransaction(
            [&]
            {
                Statement update(transactionContext.database,
                                 "UPDATE user_account SET username=?,version=version+1 "
                                 "WHERE id=? AND deleted=0");
                update.bind(1, username);
                update.bind(2, id);
                update.execute();
                if (update.rowsAffected() == 0)
                    throw std::out_of_range("user");
                Statement credential(transactionContext.database,
                                     "INSERT INTO user_credential(user_id,password_hash) "
                                     "VALUES(?,?) ON CONFLICT(user_id) DO UPDATE SET "
                                     "password_hash=excluded.password_hash");
                credential.bind(1, id);
                credential.bind(2, passwordHash);
                credential.execute();
            });
    }
    catch (const std::out_of_range&)
    {
        return AccountWriteResult::NotFound;
    }
    catch (const std::exception&)
    {
        const auto existing = findByLoginName(username);
        if (existing && existing->id != id)
            return AccountWriteResult::UsernameExists;
        throw;
    }
    updated = *findById(id);
    return AccountWriteResult::Success;
}

// 改密（CAS）：仅当传入的当前哈希与库中一致才更新——并发改密时后到者失败。
AccountWriteResult
PostgresRepository::replacePasswordHash(const std::int64_t id,
                                        const std::string_view expectedCurrentHash,
                                        const std::string_view newPasswordHash)
{
    return useDatabase(this, config_,
                       [&](QSqlDatabase* database)
                       {
                           Statement update(database, "UPDATE user_credential SET password_hash=? "
                                                      "WHERE user_id=? AND password_hash=?");
                           update.bind(1, newPasswordHash);
                           update.bind(2, id);
                           update.bind(3, expectedCurrentHash);
                           update.execute();
                           if (update.rowsAffected() > 0)
                               return AccountWriteResult::Success;
                           return findById(id) ? AccountWriteResult::VersionConflict
                                               : AccountWriteResult::NotFound;
                       });
}

AccountWriteResult PostgresRepository::updateAvatar(const std::int64_t id, AvatarData avatar,
                                                    UserAccount& updated)
{
    if (!findById(id))
        return AccountWriteResult::NotFound;
    withTransaction(
        [&]
        {
            Statement statement(
                transactionContext.database,
                "INSERT INTO user_avatar(user_id,data,content_type,etag) "
                "VALUES(?,?,?,?) ON CONFLICT(user_id) DO UPDATE SET "
                "data=excluded.data,content_type=excluded.content_type,etag=excluded."
                "etag");
            statement.bind(1, id);
            statement.bindBlob(2, avatar.bytes);
            statement.bind(3, avatar.contentType);
            statement.bind(4, avatar.etag);
            statement.execute();
            Statement version(transactionContext.database,
                              "UPDATE user_account SET version=version+1 WHERE id=? AND deleted=0");
            version.bind(1, id);
            version.execute();
        });
    updated = *findById(id);
    return AccountWriteResult::Success;
}

// 注销（UC-U-13）：有活动流程时拒绝（ActiveFlowExists）；成功则用户名/手机号
// 换成不可逆占位 deleted_user_<id> 并删除凭据与头像，主键保留供历史订单引用。
AccountWriteResult PostgresRepository::anonymize(const std::int64_t id, UserAccount& updated)
{
    AccountWriteResult result = AccountWriteResult::NotFound;
    withTransaction(
        [&]
        {
            Statement current(transactionContext.database,
                              "SELECT deleted,has_active_flow FROM user_account "
                              "WHERE id=? FOR UPDATE");
            current.bind(1, id);
            if (!current.row() || current.integer(0) != 0)
                return;
            if (current.integer(1) != 0)
            {
                result = AccountWriteResult::ActiveFlowExists;
                return;
            }
            const std::string suffix = std::to_string(id);
            Statement statement(transactionContext.database,
                                "UPDATE user_account SET "
                                "username=?,phone=?,nickname='已注销用户',status=0,"
                                "deleted=1,version=version+1 WHERE id=? AND deleted=0");
            statement.bind(1, "deleted_user_" + suffix);
            statement.bind(2, "deleted_phone_" + suffix);
            statement.bind(3, id);
            statement.execute();
            Statement credential(transactionContext.database,
                                 "DELETE FROM user_credential WHERE user_id=?");
            credential.bind(1, id);
            credential.execute();
            Statement avatar(transactionContext.database,
                             "DELETE FROM user_avatar WHERE user_id=?");
            avatar.bind(1, id);
            avatar.execute();
            result = AccountWriteResult::Success;
        });
    if (result != AccountWriteResult::Success)
        return result;
    updated.id = id;
    updated.deleted = true;
    return result;
}

void PostgresRepository::applyWalletState(const std::int64_t userId, const std::int64_t balanceCent,
                                          const std::int64_t debtCent)
{
    useDatabase(this, config_,
                [&](QSqlDatabase* database)
                {
                    Statement update(
                        database, "UPDATE user_account SET balance_cent=?,debt_cent=? WHERE id=?");
                    update.bind(1, balanceCent);
                    update.bind(2, debtCent);
                    update.bind(3, userId);
                    update.execute();
                });
}

void PostgresRepository::setActiveFlowFlag(const std::int64_t userId, const bool hasActiveFlow)
{
    useDatabase(this, config_,
                [&](QSqlDatabase* database)
                {
                    Statement update(database,
                                     "UPDATE user_account SET has_active_flow=? WHERE id=?");
                    update.bind(1, hasActiveFlow ? 1 : 0);
                    update.bind(2, userId);
                    update.execute();
                });
}

// ---- 事务入口 ----
// 写事务使用 READ COMMITTED + 显式行锁，读事务使用 REPEATABLE READ READ ONLY；
// work 抛出任何异常即 ROLLBACK 并原样上抛，保证"失败不部分成功"。
// 已在事务内时直接执行（嵌套并入外层事务）。
void PostgresRepository::withTransaction(const std::function<void()>& work)
{
    if (transactionContext.owner == this)
    {
        work();
        return;
    }
    Connection connection(config_, &connectionSlots_);
    execute(connection.get(), "BEGIN ISOLATION LEVEL READ COMMITTED");
    transactionContext = {this, connection.get(), true};
    try
    {
        work();
        execute(connection.get(), "COMMIT");
        transactionContext = {};
    }
    catch (...)
    {
        transactionContext = {};
        try
        {
            execute(connection.get(), "ROLLBACK");
        }
        catch (...)
        {
        }
        throw;
    }
}

void PostgresRepository::withReadTransaction(const std::function<void()>& work)
{
    if (transactionContext.owner == this)
    {
        work();
        return;
    }
    Connection connection(config_, &connectionSlots_);
    execute(connection.get(), "BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY");
    transactionContext = {this, connection.get(), false};
    try
    {
        work();
        execute(connection.get(), "COMMIT");
        transactionContext = {};
    }
    catch (...)
    {
        transactionContext = {};
        try
        {
            execute(connection.get(), "ROLLBACK");
        }
        catch (...)
        {
        }
        throw;
    }
}

// ---- 钱包域：当前值 + 不可变账本 + 充值单 ----
// wallet_account 存当前余额/欠费（真相），wallet_transaction 逐笔append账本，
// user_account 的余额/欠费列只是镜像；充值/结算由服务层在同一事务内三处同写。
WalletAccount PostgresRepository::wallet(const std::int64_t userId)
{
    return useDatabase(
        this, config_,
        [&](QSqlDatabase* database)
        {
            // Match createFlow: user first, wallet second. Updating the user mirror
            // after locking only the wallet would invert this order during recharge.
            if (!forUpdate(this).empty())
            {
                Statement owner(database, "SELECT id FROM user_account WHERE id=? FOR UPDATE");
                owner.bind(1, userId);
                if (!owner.row())
                    throw std::runtime_error("wallet owner not found");
            }
            const std::string sql = "SELECT user_id,balance_cent,debt_cent,version,updated_at "
                                    "FROM wallet_account WHERE user_id=?" +
                                    forUpdate(this);
            Statement query(database, sql.c_str());
            query.bind(1, userId);
            if (!query.row())
                throw std::runtime_error("wallet not found");
            return WalletAccount{query.integer(0), query.integer(1), query.integer(2),
                                 query.integer(3), query.integer(4)};
        });
}

void PostgresRepository::saveWallet(const WalletAccount& walletValue)
{
    useDatabase(this, config_,
                [&](QSqlDatabase* database)
                {
                    Statement update(
                        database,
                        "UPDATE wallet_account SET "
                        "balance_cent=?,debt_cent=?,version=?,updated_at=? WHERE user_id=?");
                    update.bind(1, walletValue.balanceCent);
                    update.bind(2, walletValue.debtCent);
                    update.bind(3, walletValue.version);
                    update.bind(4, walletValue.updatedAt);
                    update.bind(5, walletValue.userId);
                    update.execute();
                    if (update.rowsAffected() == 0)
                        throw std::runtime_error("wallet update failed");
                });
}

void PostgresRepository::addWalletTransaction(const WalletTransaction& value)
{
    useDatabase(this, config_,
                [&](QSqlDatabase* database)
                {
                    Statement insert(database,
                                     "INSERT INTO "
                                     "wallet_transaction(user_id,transaction_no,type,amount_"
                                     "cent,balance_after_cent,debt_after_cent,related_no,"
                                     "created_at) VALUES(?,?,?,?,?,?,?,?)");
                    insert.bind(1, value.userId);
                    insert.bind(2, value.transactionNo);
                    insert.bind(3, static_cast<int>(value.type));
                    insert.bind(4, value.amountCent);
                    insert.bind(5, value.balanceAfterCent);
                    insert.bind(6, value.debtAfterCent);
                    insert.bind(7, value.relatedNo);
                    insert.bind(8, value.createdAt);
                    insert.execute();
                });
}

std::vector<WalletTransaction>
PostgresRepository::walletTransactions(const std::int64_t userId,
                                       const std::optional<WalletTransactionType> type,
                                       const std::int64_t fromAt, const std::int64_t toAt)
{
    return useDatabase(
        this, config_,
        [&](QSqlDatabase* database)
        {
            Statement query(database,
                            "SELECT "
                            "id,user_id,transaction_no,type,amount_cent,balance_after_cent,debt_"
                            "after_cent,related_no,created_at FROM wallet_transaction WHERE "
                            "user_id=? AND (CAST(? AS INTEGER) IS NULL OR type=?) AND "
                            "created_at>=? AND (?=0 OR "
                            "created_at<=?) ORDER BY created_at DESC,id DESC");
            query.bind(1, userId);
            if (type)
                query.bind(2, static_cast<int>(*type));
            else
                query.bindNull(2);
            if (type)
                query.bind(3, static_cast<int>(*type));
            else
                query.bindNull(3);
            query.bind(4, fromAt);
            query.bind(5, toAt);
            query.bind(6, toAt);
            std::vector<WalletTransaction> values;
            while (query.row())
                values.push_back(WalletTransaction{
                    query.integer(0), query.integer(1), query.text(2),
                    static_cast<WalletTransactionType>(query.integer(3)), query.integer(4),
                    query.integer(5), query.integer(6), query.text(7), query.integer(8)});
            return values;
        });
}

void PostgresRepository::addRechargeOrder(const RechargeOrder& value)
{
    useDatabase(this, config_,
                [&](QSqlDatabase* database)
                {
                    Statement insert(database,
                                     "INSERT INTO recharge_order VALUES(?,?,?,?,?,?,?,?)");
                    insert.bind(1, value.rechargeNo);
                    insert.bind(2, value.userId);
                    insert.bind(3, value.requestedCent);
                    insert.bind(4, value.debtPaidCent);
                    insert.bind(5, value.balanceAddedCent);
                    insert.bind(6, value.balanceAfterCent);
                    insert.bind(7, value.debtAfterCent);
                    insert.bind(8, value.completedAt);
                    insert.execute();
                });
}

// ---- 站点/设备/费率域（读取路径）----
// 设备当前状态以 charger.status 为真相；生效费率按 adcode+时间窗取最新版本。
std::vector<Station> PostgresRepository::stations()
{
    return useDatabase(this, config_,
                       [&](QSqlDatabase* database)
                       {
                           Statement query(
                               database, "SELECT "
                                         "id,code,name,address,adcode,latitude_e6,longitude_e6,"
                                         "business_hours,enabled,version FROM station ORDER BY id");
                           std::vector<Station> values;
                           while (query.row())
                               values.push_back(readStation(query));
                           return values;
                       });
}

std::optional<Station> PostgresRepository::station(const std::int64_t stationId)
{
    return useDatabase(
        this, config_,
        [&](QSqlDatabase* database) -> std::optional<Station>
        {
            Statement query(database, "SELECT "
                                      "id,code,name,address,adcode,latitude_e6,longitude_e6,"
                                      "business_hours,enabled,version FROM station WHERE id=?");
            query.bind(1, stationId);
            return query.row() ? std::optional<Station>(readStation(query)) : std::nullopt;
        });
}

std::vector<Charger> PostgresRepository::chargers(const std::optional<std::int64_t> stationId,
                                                  const std::optional<ChargerType> type,
                                                  const std::optional<ChargerStatus> status)
{
    return useDatabase(this, config_,
                       [&](QSqlDatabase* database)
                       {
                           const std::string sql =
                               "SELECT id,station_id,code,charger_type,power_watt,connector_"
                               "standard,status,total_count,total_minutes,version FROM charger "
                               "WHERE (CAST(? AS BIGINT) IS NULL OR station_id=?) AND "
                               "(CAST(? AS INTEGER) IS NULL OR charger_type=?) AND "
                               "(CAST(? AS INTEGER) IS NULL OR status=?) ORDER BY id" +
                               forUpdate(this, true);
                           Statement query(database, sql.c_str());
                           if (stationId)
                               query.bind(1, *stationId);
                           else
                               query.bindNull(1);
                           if (stationId)
                               query.bind(2, *stationId);
                           else
                               query.bindNull(2);
                           if (type)
                               query.bind(3, static_cast<int>(*type));
                           else
                               query.bindNull(3);
                           if (type)
                               query.bind(4, static_cast<int>(*type));
                           else
                               query.bindNull(4);
                           if (status)
                               query.bind(5, static_cast<int>(*status));
                           else
                               query.bindNull(5);
                           if (status)
                               query.bind(6, static_cast<int>(*status));
                           else
                               query.bindNull(6);
                           std::vector<Charger> values;
                           while (query.row())
                               values.push_back(readCharger(query));
                           return values;
                       });
}

std::optional<Charger> PostgresRepository::charger(const std::int64_t chargerId)
{
    return useDatabase(
        this, config_,
        [&](QSqlDatabase* database) -> std::optional<Charger>
        {
            const std::string sql =
                "SELECT id,station_id,code,charger_type,power_watt,connector_standard,"
                "status,total_count,total_minutes,version FROM charger WHERE id=?" +
                forUpdate(this);
            Statement query(database, sql.c_str());
            query.bind(1, chargerId);
            return query.row() ? std::optional<Charger>(readCharger(query)) : std::nullopt;
        });
}

void PostgresRepository::saveCharger(const Charger& value)
{
    useDatabase(this, config_,
                [&](QSqlDatabase* database)
                {
                    Statement update(
                        database,
                        "UPDATE charger SET "
                        "station_id=?,code=?,charger_type=?,power_watt=?,connector_standard=?,"
                        "status=?,total_count=?,total_minutes=?,version=? WHERE id=?");
                    update.bind(1, value.stationId);
                    update.bind(2, value.code);
                    update.bind(3, static_cast<int>(value.type));
                    update.bind(4, value.powerWatt);
                    update.bind(5, value.connectorStandard);
                    update.bind(6, static_cast<int>(value.status));
                    update.bind(7, value.totalCount);
                    update.bind(8, value.totalMinutes);
                    update.bind(9, value.version);
                    update.bind(10, value.id);
                    update.execute();
                });
}

// 生效费率：取 adcode 命中 [effective_from, effective_to] 时间窗的最新版本；
// 无命中返回 nullopt，由调用方决定报错或回退。
std::optional<RegionTariff> PostgresRepository::effectiveTariff(const std::string& adcode,
                                                                const std::int64_t at)
{
    return useDatabase(this, config_,
                       [&](QSqlDatabase* database) -> std::optional<RegionTariff>
                       {
                           Statement query(database,
                                           "SELECT "
                                           "adcode,electricity_cent_per_kwh,service_cent_per_kwh,"
                                           "effective_from,effective_to FROM region_tariff WHERE "
                                           "adcode=? AND effective_from<=? AND effective_to>=? "
                                           "ORDER BY effective_from DESC LIMIT 1");
                           query.bind(1, adcode);
                           query.bind(2, at);
                           query.bind(3, at);
                           if (!query.row())
                               return std::nullopt;
                           return RegionTariff{query.text(0), static_cast<int>(query.integer(1)),
                                               static_cast<int>(query.integer(2)), query.integer(3),
                                               query.integer(4)};
                       });
}

} // namespace ncs::infrastructure::postgres
