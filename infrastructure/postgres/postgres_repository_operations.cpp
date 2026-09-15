#include "infrastructure/postgres/postgres_backup_detail.h"
#include "infrastructure/postgres/postgres_repository.h"
#include "infrastructure/postgres/postgres_row_mappers.h"

namespace ncs::infrastructure::postgres
{

using namespace ncs::core::application;
using namespace detail;

// ---- ML 任务域：训练/预测任务的互斥、登记与超时回收 ----
std::optional<MlTask> PostgresRepository::runningMlTask(const std::string& taskType)
{
    return useDatabase(
        this, config_,
        [&](QSqlDatabase* database) -> std::optional<MlTask>
        {
            Statement query(database,
                            "SELECT task_no,task_type,status,model_version,horizon_hours,"
                            "created_at,finished_at,metrics_summary,error_summary FROM ml_task "
                            "WHERE task_type=? AND status IN ('PENDING','RUNNING') "
                            "ORDER BY created_at DESC LIMIT 1");
            query.bind(1, taskType);
            return query.row() ? std::optional<MlTask>(readMlTask(query)) : std::nullopt;
        });
}

std::vector<MlTask> PostgresRepository::overdueMlTasks(const std::int64_t trainDeadline,
                                                       const std::int64_t predictDeadline)
{
    return useDatabase(this, config_,
                       [&](QSqlDatabase* database)
                       {
                           Statement query(
                               database,
                               "SELECT task_no,task_type,status,model_version,horizon_hours,"
                               "created_at,finished_at,metrics_summary,error_summary FROM ml_task "
                               "WHERE status IN ('PENDING','RUNNING') AND "
                               "((task_type='TRAIN' AND created_at<=?) OR "
                               "(task_type='PREDICT' AND created_at<=?)) "
                               "ORDER BY created_at,task_no LIMIT 100");
                           query.bind(1, trainDeadline);
                           query.bind(2, predictDeadline);
                           std::vector<MlTask> tasks;
                           while (query.row())
                               tasks.push_back(readMlTask(query));
                           return tasks;
                       });
}

void PostgresRepository::addMlTask(const MlTask& task)
{
    useDatabase(this, config_,
                [&](QSqlDatabase* database)
                {
                    Statement insert(
                        database,
                        "INSERT INTO ml_task(task_no,task_type,status,model_version,"
                        "horizon_hours,created_at,finished_at,metrics_summary,error_summary) "
                        "VALUES(?,?,?,?,?,?,?,?,?)");
                    insert.bind(1, task.taskNo);
                    insert.bind(2, task.taskType);
                    insert.bind(3, task.status);
                    insert.bind(4, task.modelVersion);
                    insert.bind(5, encodeHorizons(task.horizonHours));
                    insert.bind(6, task.createdAt);
                    bindOptional(insert, 7, task.finishedAt);
                    insert.bind(8, task.metricsSummary);
                    insert.bind(9, task.errorSummary);
                    insert.execute();
                });
}

std::optional<MlTask> PostgresRepository::mlTask(const std::string& taskNo)
{
    return useDatabase(
        this, config_,
        [&](QSqlDatabase* database) -> std::optional<MlTask>
        {
            Statement query(database,
                            "SELECT task_no,task_type,status,model_version,horizon_hours,"
                            "created_at,finished_at,metrics_summary,error_summary FROM ml_task "
                            "WHERE task_no=?");
            query.bind(1, taskNo);
            return query.row() ? std::optional<MlTask>(readMlTask(query)) : std::nullopt;
        });
}

void PostgresRepository::saveMlTask(const MlTask& task)
{
    useDatabase(this, config_,
                [&](QSqlDatabase* database)
                {
                    Statement update(
                        database,
                        "UPDATE ml_task SET status=?,model_version=?,horizon_hours=?,"
                        "finished_at=?,metrics_summary=?,error_summary=? WHERE task_no=?");
                    update.bind(1, task.status);
                    update.bind(2, task.modelVersion);
                    update.bind(3, encodeHorizons(task.horizonHours));
                    bindOptional(update, 4, task.finishedAt);
                    update.bind(5, task.metricsSummary);
                    update.bind(6, task.errorSummary);
                    update.bind(7, task.taskNo);
                    update.execute();
                });
}

// 任务完成（CAS）：仅当任务仍处于 PENDING/RUNNING（allowTimedOut 时含
// TIMED_OUT）才允许写入终态，返回 false 表示被并发抢先或已超时。
bool PostgresRepository::tryFinishMlTask(const MlTask& task, const bool allowTimedOut)
{
    return useDatabase(this, config_,
                       [&](QSqlDatabase* database)
                       {
                           const char* sql =
                               allowTimedOut ? "UPDATE ml_task SET status=?,model_version=?,"
                                               "horizon_hours=?,finished_at=?,metrics_summary=?,"
                                               "error_summary=? WHERE task_no=? AND status IN "
                                               "('PENDING','RUNNING','TIMED_OUT')"
                                             : "UPDATE ml_task SET status=?,model_version=?,"
                                               "horizon_hours=?,finished_at=?,metrics_summary=?,"
                                               "error_summary=? WHERE task_no=? AND status IN "
                                               "('PENDING','RUNNING')";
                           Statement update(database, sql);
                           update.bind(1, task.status);
                           update.bind(2, task.modelVersion);
                           update.bind(3, encodeHorizons(task.horizonHours));
                           bindOptional(update, 4, task.finishedAt);
                           update.bind(5, task.metricsSummary);
                           update.bind(6, task.errorSummary);
                           update.bind(7, task.taskNo);
                           update.execute();
                           return update.rowsAffected() == 1;
                       });
}

// ---- 备份域：元数据记录 + Online Backup 快照 + 校验 ----
// createBackupSnapshot 走 PostgreSQL Online Backup API（禁止运行时直接拷文件），
// verifyBackupSnapshot 在隔离副本上跑 integrity_check 并比对 SHA-256 与大小。
void PostgresRepository::addBackup(const BackupRecord& record)
{
    useDatabase(this, config_,
                [&](QSqlDatabase* database)
                {
                    Statement insert(
                        database, "INSERT INTO backup_record(backup_no,status,checksum,size_bytes,"
                                  "created_at,verification_status,verified_at,storage_path) "
                                  "VALUES(?,?,?,?,?,?,?,?)");
                    insert.bind(1, record.backupNo);
                    insert.bind(2, record.status);
                    insert.bind(3, record.checksum);
                    insert.bind(4, record.sizeBytes);
                    insert.bind(5, record.createdAt);
                    insert.bind(6, record.verificationStatus);
                    bindOptional(insert, 7, record.verifiedAt);
                    insert.bind(8, record.storagePath);
                    insert.execute();
                });
}

std::vector<BackupRecord> PostgresRepository::backups()
{
    return useDatabase(this, config_,
                       [&](QSqlDatabase* database)
                       {
                           Statement query(
                               database,
                               "SELECT backup_no,status,checksum,size_bytes,created_at,"
                               "verification_status,verified_at,storage_path FROM backup_record "
                               "ORDER BY created_at DESC,backup_no DESC");
                           std::vector<BackupRecord> records;
                           while (query.row())
                           {
                               BackupRecord record;
                               record.backupNo = query.text(0);
                               record.status = query.text(1);
                               record.checksum = query.text(2);
                               record.sizeBytes = query.integer(3);
                               record.createdAt = query.integer(4);
                               record.verificationStatus = query.text(5);
                               record.verifiedAt = optionalInteger(query, 6);
                               record.storagePath = query.text(7);
                               records.push_back(std::move(record));
                           }
                           return records;
                       });
}

std::optional<BackupRecord> PostgresRepository::backup(const std::string& backupNo)
{
    return useDatabase(this, config_,
                       [&](QSqlDatabase* database) -> std::optional<BackupRecord>
                       {
                           Statement query(
                               database,
                               "SELECT backup_no,status,checksum,size_bytes,created_at,"
                               "verification_status,verified_at,storage_path FROM backup_record "
                               "WHERE backup_no=?");
                           query.bind(1, backupNo);
                           if (!query.row())
                               return std::nullopt;
                           BackupRecord record;
                           record.backupNo = query.text(0);
                           record.status = query.text(1);
                           record.checksum = query.text(2);
                           record.sizeBytes = query.integer(3);
                           record.createdAt = query.integer(4);
                           record.verificationStatus = query.text(5);
                           record.verifiedAt = optionalInteger(query, 6);
                           record.storagePath = query.text(7);
                           return record;
                       });
}

void PostgresRepository::saveBackup(const BackupRecord& record)
{
    useDatabase(this, config_,
                [&](QSqlDatabase* database)
                {
                    Statement update(
                        database,
                        "UPDATE backup_record SET status=?,checksum=?,size_bytes=?,"
                        "verification_status=?,verified_at=?,storage_path=? WHERE backup_no=?");
                    update.bind(1, record.status);
                    update.bind(2, record.checksum);
                    update.bind(3, record.sizeBytes);
                    update.bind(4, record.verificationStatus);
                    bindOptional(update, 5, record.verifiedAt);
                    update.bind(6, record.storagePath);
                    update.bind(7, record.backupNo);
                    update.execute();
                });
}

// 逻辑备份：备份号做白名单校验后落到受控目录，经 pg_dump custom format 生成一致性
// 快照，完成后记录 SHA-256 与大小；凭据只经子进程环境传递，任一步失败删除半成品。
bool PostgresRepository::createBackupSnapshot(BackupRecord& record)
{
    if (record.backupNo.empty() ||
        record.backupNo.find_first_not_of(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") !=
            std::string::npos)
    {
        return false;
    }
    const std::filesystem::path directory(config_.backupDirectory);
    if (config_.backupDirectory.empty())
        return false;
    std::filesystem::create_directories(directory);
    if (!restrictOwnerPermissions(directory, true))
        return false;
    const std::filesystem::path destinationPath = directory / (record.backupNo + ".dump");
    if (std::filesystem::exists(destinationPath))
        return false;

    const QStringList arguments{
        QStringLiteral("--format=custom"),
        QStringLiteral("--no-owner"),
        QStringLiteral("--no-privileges"),
        QStringLiteral("--no-password"),
        QStringLiteral("--host"),
        QString::fromStdString(config_.host),
        QStringLiteral("--port"),
        QString::number(config_.port),
        QStringLiteral("--username"),
        QString::fromStdString(config_.user),
        QStringLiteral("--file"),
        QString::fromStdString(destinationPath.string()),
        QString::fromStdString(config_.database),
    };
    const bool succeeded = runPostgresTool(config_.pgDumpExecutable, arguments, config_, 300000);
    if (!succeeded)
    {
        std::error_code ignored;
        std::filesystem::remove(destinationPath, ignored);
        return false;
    }

    try
    {
        if (!restrictOwnerPermissions(destinationPath, false))
            throw std::runtime_error("backup permissions could not be restricted");
        record.storagePath = std::filesystem::absolute(destinationPath).string();
        record.sizeBytes = static_cast<std::int64_t>(std::filesystem::file_size(destinationPath));
        record.checksum = fileSha256(destinationPath);
        return record.sizeBytes > 0 && !record.checksum.empty();
    }
    catch (...)
    {
        std::error_code ignored;
        std::filesystem::remove(destinationPath, ignored);
        record.storagePath.clear();
        record.sizeBytes = 0;
        record.checksum.clear();
        return false;
    }
}

// 快速备份验证：先限定受控目录并比对大小/SHA-256，再由 pg_restore --list 校验
// custom archive 的目录可读性（不证明所有数据块可恢复）；完整恢复需在隔离库实际执行。
bool PostgresRepository::verifyBackupSnapshot(const BackupRecord& record)
{
    if (record.storagePath.empty() || record.checksum.empty() || record.sizeBytes <= 0)
        return false;
    const std::filesystem::path backupPath(record.storagePath);
    if (config_.backupDirectory.empty())
        return false;
    const std::filesystem::path allowedDirectory =
        std::filesystem::weakly_canonical(std::filesystem::path(config_.backupDirectory));
    std::error_code pathError;
    const std::filesystem::path canonicalBackup =
        std::filesystem::weakly_canonical(backupPath, pathError);
    if (pathError || canonicalBackup.parent_path() != allowedDirectory ||
        !std::filesystem::is_regular_file(canonicalBackup) ||
        static_cast<std::int64_t>(std::filesystem::file_size(canonicalBackup)) !=
            record.sizeBytes ||
        fileSha256(canonicalBackup) != record.checksum)
    {
        return false;
    }

    const QStringList arguments{QStringLiteral("--list"),
                                QString::fromStdString(canonicalBackup.string())};
    return runPostgresTool(config_.pgRestoreExecutable, arguments, config_, 120000);
}

// ---- 保留清理域：审计/命令 180 天、已投递 Outbox 7 天、死信 30 天（NFR-R-03）----
void PostgresRepository::cleanupAdminRecords(const std::int64_t now)
{
    constexpr std::int64_t retention = 180LL * 24 * 3600;
    constexpr std::int64_t deliveredOutboxRetention = 7LL * 24 * 3600;
    constexpr std::int64_t deadOutboxRetention = 30LL * 24 * 3600;
    useDatabase(
        this, config_,
        [&](QSqlDatabase* database)
        {
            Statement audit(database, "DELETE FROM ops_log WHERE at<?");
            audit.bind(1, now - retention);
            audit.execute();
            Statement commands(
                database,
                "DELETE FROM device_command WHERE completed_at IS NOT NULL AND completed_at<?");
            commands.bind(1, now - retention);
            commands.execute();
            Statement deliveredOutbox(database,
                                      "DELETE FROM outbox_event WHERE delivery_status=1 AND "
                                      "published_at IS NOT NULL AND published_at<?");
            deliveredOutbox.bind(1, now - deliveredOutboxRetention);
            deliveredOutbox.execute();
            Statement deadOutbox(
                database, "DELETE FROM outbox_event WHERE delivery_status=2 AND created_at<?");
            deadOutbox.bind(1, now - deadOutboxRetention);
            deadOutbox.execute();
        });
    pruneBackups(now);
}

// ---- 大屏小时聚合重建：可重建数据，不直接种子化 ----
// 按天分块 UPSERT（避免长事务长期持有写锁导致并发写 SQLITE_BUSY），
// 每个桶在重建期间始终可读。
void PostgresRepository::refreshHourlyMetrics(const std::int64_t fromAt, const std::int64_t toAt)
{
    if (fromAt < 0 || toAt <= fromAt)
        throw std::invalid_argument("invalid hourly metric range");
    const std::int64_t firstHour = fromAt / 3600 * 3600;
    // toAt is exclusive and only fully completed UTC hours are materialized.
    const std::int64_t exclusiveHour = toAt / 3600 * 3600;
    if (exclusiveHour <= firstHour)
        return;
    const std::int64_t lastHour = exclusiveHour - 3600;
    const std::int64_t refreshedAt = std::chrono::duration_cast<std::chrono::seconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count();
    // The rebuild is split into per-day UPSERT transactions. A single 90-day
    // DELETE+INSERT inside one BEGIN IMMEDIATE held the write lock long enough
    // to fail concurrent writers with SQLITE_BUSY; per-day chunks bound each
    // write-lock window, and UPSERT (instead of delete+insert) keeps every
    // bucket readable while the rebuild is in progress.
    constexpr std::int64_t kChunk = 24 * 3600;
    for (std::int64_t chunkStart = firstHour; chunkStart <= lastHour; chunkStart += kChunk)
    {
        const std::int64_t chunkEnd = std::min(chunkStart + kChunk - 3600, lastHour);
        withTransaction(
            [&]
            {
                useDatabase(
                    this, config_,
                    [&](QSqlDatabase* database)
                    {
                        Statement upsert(
                            database,
                            "WITH RECURSIVE hours(bucket_at) AS (SELECT CAST(? AS BIGINT) UNION "
                            "ALL "
                            "SELECT bucket_at+3600 FROM hours WHERE bucket_at+3600<=?), "
                            "aggregates AS (SELECT station_id,"
                            "(COALESCE(started_at,created_at)/3600)*3600 AS bucket_at,"
                            "SUM(energy_mwh) AS energy_mwh,COUNT(*) AS order_count,"
                            "SUM(CASE WHEN charger_type=1 THEN 1 ELSE 0 END) AS fast_count,"
                            "SUM(CASE WHEN charger_type=0 THEN 1 ELSE 0 END) AS slow_count,"
                            "SUM(CASE WHEN ended_at IS NOT NULL AND started_at IS NOT NULL "
                            "THEN GREATEST(0,ended_at-started_at) ELSE 0 END) AS busy_seconds "
                            "FROM charging_order WHERE status=? AND "
                            "COALESCE(started_at,created_at)>=? AND "
                            "COALESCE(started_at,created_at)<? GROUP BY station_id,bucket_at) "
                            "INSERT INTO station_hourly_metric(station_id,bucket_at,energy_mwh,"
                            "order_count,fast_order_count,slow_order_count,busy_device_seconds,"
                            "refreshed_at) SELECT s.id,h.bucket_at,COALESCE(a.energy_mwh,0),"
                            "COALESCE(a.order_count,0),COALESCE(a.fast_count,0),"
                            "COALESCE(a.slow_count,0),COALESCE(a.busy_seconds,0),? "
                            "FROM station s CROSS JOIN hours h LEFT JOIN aggregates a ON "
                            "a.station_id=s.id AND a.bucket_at=h.bucket_at "
                            "ON CONFLICT(station_id,bucket_at) DO UPDATE SET "
                            "energy_mwh=excluded.energy_mwh,order_count=excluded.order_count,"
                            "fast_order_count=excluded.fast_order_count,"
                            "slow_order_count=excluded.slow_order_count,"
                            "busy_device_seconds=excluded.busy_device_seconds,"
                            "refreshed_at=excluded.refreshed_at");
                        upsert.bind(1, chunkStart);
                        upsert.bind(2, chunkEnd);
                        upsert.bind(3, static_cast<int>(FlowStatus::Completed));
                        upsert.bind(4, chunkStart);
                        upsert.bind(5, chunkEnd + 3600);
                        upsert.bind(6, refreshedAt);
                        upsert.execute();
                    });
            });
    }
}

// 小时聚合查询：bucket+station 复合游标分页（limit+1 探测下一页），
// 同时返回该站当前可运营（非故障非停用）设备数。
HourlyMetricPage PostgresRepository::hourlyMetrics(const std::int64_t fromAt,
                                                   const std::int64_t toAt,
                                                   const std::optional<std::int64_t> stationId,
                                                   const std::string_view cursor, const int limit)
{
    std::int64_t cursorBucket = -1;
    std::int64_t cursorStation = -1;
    if (!cursor.empty())
    {
        const auto separator = cursor.find(':');
        if (separator == std::string_view::npos)
            throw std::invalid_argument("invalid metric cursor");
        const auto left = cursor.substr(0, separator);
        const auto right = cursor.substr(separator + 1);
        const auto first = std::from_chars(left.data(), left.data() + left.size(), cursorBucket);
        const auto second =
            std::from_chars(right.data(), right.data() + right.size(), cursorStation);
        if (first.ec != std::errc{} || first.ptr != left.data() + left.size() ||
            second.ec != std::errc{} || second.ptr != right.data() + right.size() ||
            cursorBucket < 0 || cursorStation < 0)
            throw std::invalid_argument("invalid metric cursor");
    }
    return useDatabase(
        this, config_,
        [&](QSqlDatabase* database)
        {
            Statement query(database,
                            "SELECT m.station_id,s.name,m.bucket_at,m.energy_mwh,m.order_count,"
                            "m.fast_order_count,m.slow_order_count,"
                            "m.busy_device_seconds,"
                            "(SELECT COUNT(*) FROM charger c WHERE c.station_id=m.station_id "
                            "AND c.status IN (0,1)) "
                            "FROM station_hourly_metric m JOIN station s ON s.id=m.station_id "
                            "WHERE m.bucket_at>=? AND m.bucket_at<=? AND "
                            "(CAST(? AS BIGINT) IS NULL OR m.station_id=?) "
                            "AND (m.bucket_at>? OR (m.bucket_at=? AND m.station_id>?)) "
                            "ORDER BY m.bucket_at,m.station_id LIMIT ?");
            query.bind(1, fromAt / 3600 * 3600);
            query.bind(2, toAt / 3600 * 3600);
            if (stationId)
            {
                query.bind(3, *stationId);
                query.bind(4, *stationId);
            }
            else
            {
                query.bindNull(3);
                query.bindNull(4);
            }
            query.bind(5, cursorBucket);
            query.bind(6, cursorBucket);
            query.bind(7, cursorStation);
            query.bind(8, limit + 1);
            HourlyMetricPage page;
            while (query.row())
            {
                page.items.push_back(HourlyMetric{
                    query.integer(0), query.text(1), query.integer(2), query.integer(3),
                    static_cast<int>(query.integer(4)), static_cast<int>(query.integer(5)),
                    static_cast<int>(query.integer(6)), static_cast<int>(query.integer(8)),
                    query.integer(7)});
            }
            if (page.items.size() > static_cast<std::size_t>(limit))
            {
                page.items.resize(static_cast<std::size_t>(limit));
                const auto& last = page.items.back();
                page.nextCursor =
                    std::to_string(last.bucketAt) + ":" + std::to_string(last.stationId);
            }
            return page;
        });
}

} // namespace ncs::infrastructure::postgres
