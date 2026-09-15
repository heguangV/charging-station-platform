#include "infrastructure/postgres/postgres_repository.h"
#include "infrastructure/postgres/postgres_repository_detail.h"

namespace ncs::infrastructure::postgres
{

using namespace ncs::core::application;
using namespace detail;

ReadinessStatus PostgresRepository::check()
{
    std::lock_guard lock(readinessMutex_);
    return readiness_;
}

void PostgresRepository::refreshReadiness()
{
    const ReadinessStatus latest = probeDatabase();
    std::lock_guard lock(readinessMutex_);
    readiness_ = latest;
}

// 就绪探针：核对 schema/checksum 与 PostgreSQL WAL 配置，并用事务内空更新验证可写；
// 任何异常返回全未就绪。
ReadinessStatus PostgresRepository::probeDatabase()
{
    ReadinessStatus status;
    try
    {
        Connection connection(config_, &connectionSlots_);
        Statement version(connection.get(), "SELECT version,checksum FROM schema_version ORDER BY "
                                            "version DESC LIMIT 1");
        status.schemaVersion = version.row() && version.integer(0) == kLatestSchemaVersion &&
                               version.text(1) == kLatestSchemaChecksum;
        Statement wal(connection.get(), "SELECT current_setting('fsync'), "
                                        "current_setting('full_page_writes'), "
                                        "current_setting('synchronous_commit')");
        // Keep the historical API field, but check local crash durability rather
        // than the always-present wal_level. This does not attest WAL archival.
        status.walEnabled = wal.row() && durableWalSettings(wal.text(0), wal.text(1), wal.text(2));
        connection.execute("BEGIN");
        Statement touch(connection.get(), "UPDATE schema_version SET applied_at=applied_at "
                                          "WHERE version=?");
        touch.bind(1, static_cast<std::int64_t>(kLatestSchemaVersion));
        touch.execute();
        connection.execute("ROLLBACK");
        status.databaseReadWrite = true;
        status.migrationsComplete = status.schemaVersion;
    }
    catch (...)
    {
        return {};
    }
    return status;
}

// ---- 业务编号与幂等域 ----
// 序号用 UPSERT...RETURNING 原子自增（前缀+UTC 日唯一）；幂等记录含请求摘要、
// 结果重放、租约与 permanent 标记，与业务写入同事务落库。
std::int64_t PostgresRepository::nextBusinessSequence(const std::string_view prefix,
                                                      const std::int64_t utcDay)
{
    return useDatabase(this, config_,
                       [&](QSqlDatabase* database)
                       {
                           Statement statement(
                               database,
                               "INSERT INTO business_sequence(prefix,utc_day,value) VALUES(?,?,1) "
                               "ON CONFLICT(prefix,utc_day) DO UPDATE SET "
                               "value=business_sequence.value+1 RETURNING business_sequence.value");
                           statement.bind(1, prefix);
                           statement.bind(2, utcDay);
                           if (!statement.row())
                               throw std::runtime_error("business sequence unavailable");
                           return statement.integer(0);
                       });
}

// 读取幂等记录：返回请求摘要、已存结果（重放用）、租约与 permanent 标记；
// 无记录返回 nullopt（首次请求）。
std::optional<PersistedIdempotencyRecord>
PostgresRepository::loadIdempotencyRecord(const std::string_view scope, const std::string_view key)
{
    return useDatabase(this, config_,
                       [&](QSqlDatabase* database) -> std::optional<PersistedIdempotencyRecord>
                       {
                           Statement query(
                               database,
                               "SELECT "
                               "request_digest,result_status,result_content_type,result_body,"
                               "expires_at,lease_expires_at,lease_token,permanent "
                               "FROM idempotency_record WHERE scope=? AND idempotency_key=?");
                           query.bind(1, scope);
                           query.bind(2, key);
                           if (!query.row())
                               return std::nullopt;
                           PersistedIdempotencyRecord record;
                           record.scope = std::string(scope);
                           record.key = std::string(key);
                           record.requestDigest = query.text(0);
                           if (!query.isNull(1))
                           {
                               record.result = StoredHttpResult{static_cast<int>(query.integer(1)),
                                                                query.text(2), query.text(3)};
                           }
                           record.expiresAt = query.integer(4);
                           record.leaseExpiresAt = query.integer(5);
                           record.leaseToken = query.text(6);
                           record.permanent = query.integer(7) != 0;
                           return record;
                       });
}

void PostgresRepository::saveIdempotencyRecord(const PersistedIdempotencyRecord& record)
{
    useDatabase(this, config_,
                [&](QSqlDatabase* database)
                {
                    Statement statement(
                        database,
                        "INSERT INTO idempotency_record(scope,idempotency_key,request_digest,"
                        "result_status,result_content_type,result_body,expires_at,lease_"
                        "expires_at,"
                        "lease_token,permanent) VALUES(?,?,?,?,?,?,?,?,?,?) "
                        "ON CONFLICT(scope,idempotency_key) DO UPDATE SET "
                        "request_digest=excluded.request_digest,result_status=excluded.result_"
                        "status,"
                        "result_content_type=excluded.result_content_type,result_body=excluded."
                        "result_body,"
                        "expires_at=excluded.expires_at,lease_expires_at=excluded.lease_"
                        "expires_at,"
                        "lease_token=excluded.lease_token,permanent=excluded.permanent");
                    statement.bind(1, record.scope);
                    statement.bind(2, record.key);
                    statement.bind(3, record.requestDigest);
                    if (record.result)
                    {
                        statement.bind(4, record.result->status);
                        statement.bind(5, record.result->contentType);
                        statement.bind(6, record.result->body);
                    }
                    else
                    {
                        statement.bindNull(4);
                        statement.bindNull(5);
                        statement.bindNull(6);
                    }
                    statement.bind(7, record.expiresAt);
                    statement.bind(8, record.leaseExpiresAt);
                    statement.bind(9, record.leaseToken);
                    statement.bind(10, record.permanent ? 1 : 0);
                    statement.execute();
                });
}

void PostgresRepository::removeIdempotencyRecord(const std::string_view scope,
                                                 const std::string_view key)
{
    useDatabase(this, config_,
                [&](QSqlDatabase* database)
                {
                    Statement statement(
                        database,
                        "DELETE FROM idempotency_record WHERE scope=? AND idempotency_key=?");
                    statement.bind(1, scope);
                    statement.bind(2, key);
                    statement.execute();
                });
}

// 幂等记录清理：未完成记录按租约到期、已完成非永久记录按 expires_at 删除；
// 充值/结算等 permanent=1 的记录不清理（契约要求业务唯一性永久保留）。
void PostgresRepository::cleanupIdempotencyRecords(const std::int64_t now)
{
    useDatabase(this, config_,
                [&](QSqlDatabase* database)
                {
                    Statement statement(
                        database, "DELETE FROM idempotency_record WHERE "
                                  "(result_status IS NULL AND lease_expires_at<=?) OR "
                                  "(result_status IS NOT NULL AND permanent=0 AND expires_at<=?)");
                    statement.bind(1, now);
                    statement.bind(2, now);
                    statement.execute();
                });
}

std::size_t PostgresRepository::idempotencyRecordCount()
{
    return useDatabase(this, config_,
                       [&](QSqlDatabase* database)
                       {
                           Statement query(database, "SELECT count(*) FROM idempotency_record");
                           if (!query.row())
                               return std::size_t{0};
                           return static_cast<std::size_t>(query.integer(0));
                       });
}

} // namespace ncs::infrastructure::postgres
