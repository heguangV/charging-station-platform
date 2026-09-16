#include "infrastructure/postgres/postgres_repository.h"
#include "infrastructure/postgres/postgres_row_mappers.h"

namespace ncs::infrastructure::postgres
{

using namespace ncs::core::application;
using namespace detail;

// ---- 大屏版本与负荷预测域：预测按站点+模型+目标时间唯一，可标记过期 ----
std::int64_t PostgresRepository::nextDashboardVersion()
{
    return useDatabase(this, config_,
                       [&](QSqlDatabase* database)
                       {
                           Statement update(
                               database, "UPDATE dashboard_state SET data_version=data_version+1 "
                                         "WHERE singleton=1 RETURNING data_version");
                           if (!update.row())
                               throw std::runtime_error("dashboard version unavailable");
                           return update.integer(0);
                       });
}

void PostgresRepository::addModelVersion(const ModelVersion& version)
{
    useDatabase(this, config_,
                [&](QSqlDatabase* database)
                {
                    Statement insert(
                        database,
                        "INSERT INTO model_version(version_no,task_no,algorithm,"
                        "feature_schema_version,random_seed,train_from_at,train_to_at,mae,rmse,"
                        "mape,wape,baseline_mae,baseline_rmse,excluded_sample_count,qualified,"
                        "artifact_checksum,artifact_path,created_at) "
                        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
                    insert.bind(1, version.versionNo);
                    insert.bind(2, version.taskNo);
                    insert.bind(3, version.algorithm);
                    insert.bind(4, version.featureSchemaVersion);
                    insert.bind(5, version.randomSeed);
                    insert.bind(6, version.trainFromAt);
                    insert.bind(7, version.trainToAt);
                    insert.bind(8, version.mae);
                    insert.bind(9, version.rmse);
                    insert.bind(10, version.mape);
                    insert.bind(11, version.wape);
                    insert.bind(12, version.baselineMae);
                    insert.bind(13, version.baselineRmse);
                    insert.bind(14, version.excludedSampleCount);
                    insert.bind(15, version.qualified ? 1 : 0);
                    insert.bind(16, version.artifactChecksum);
                    insert.bind(17, version.artifactPath);
                    insert.bind(18, version.createdAt);
                    insert.execute();
                });
}

std::optional<ModelVersion> PostgresRepository::modelVersion(const std::string_view versionNo)
{
    return useDatabase(
        this, config_,
        [&](QSqlDatabase* database) -> std::optional<ModelVersion>
        {
            Statement query(
                database, "SELECT version_no,task_no,algorithm,feature_schema_version,random_seed,"
                          "train_from_at,train_to_at,mae,rmse,mape,wape,baseline_mae,"
                          "baseline_rmse,excluded_sample_count,qualified,artifact_checksum,"
                          "artifact_path,created_at FROM model_version WHERE version_no=?");
            query.bind(1, versionNo);
            if (!query.row())
                return std::nullopt;
            return ModelVersion{query.text(0),          query.text(1),
                                query.text(2),          static_cast<int>(query.integer(3)),
                                query.integer(4),       query.integer(5),
                                query.integer(6),       query.real(7),
                                query.real(8),          query.real(9),
                                query.real(10),         query.real(11),
                                query.real(12),         static_cast<int>(query.integer(13)),
                                query.integer(14) != 0, query.text(15),
                                query.text(16),         query.integer(17)};
        });
}

std::optional<ModelVersion> PostgresRepository::latestQualifiedModel()
{
    return useDatabase(
        this, config_,
        [&](QSqlDatabase* database) -> std::optional<ModelVersion>
        {
            Statement versionNo(
                database, "SELECT v.version_no FROM model_version v JOIN ml_task t "
                          "ON t.task_no=v.task_no WHERE v.qualified=1 AND t.status='SUCCEEDED' "
                          "ORDER BY v.created_at DESC,v.version_no DESC LIMIT 1");
            if (!versionNo.row())
                return std::nullopt;
            return modelVersion(versionNo.text(0));
        });
}

void PostgresRepository::upsertPredictions(const std::vector<LoadPrediction>& items)
{
    withTransaction(
        [&]
        {
            useDatabase(
                this, config_,
                [&](QSqlDatabase* database)
                {
                    Statement upsert(
                        database,
                        "INSERT INTO load_prediction(station_id,model_version_no,generated_at,"
                        "target_at,horizon_hour,predicted_energy_mwh,predicted_free_count,"
                        "is_peak,stale) VALUES(?,?,?,?,?,?,?,?,?) ON CONFLICT(station_id,"
                        "model_version_no,target_at) DO UPDATE SET "
                        "generated_at=excluded.generated_at,"
                        "horizon_hour=excluded.horizon_hour,predicted_energy_mwh=excluded."
                        "predicted_energy_mwh,"
                        "predicted_free_count=excluded.predicted_free_count,is_peak=excluded.is_"
                        "peak,stale=excluded.stale");
                    for (const auto& item : items)
                    {
                        upsert.bind(1, item.stationId);
                        upsert.bind(2, item.modelVersionNo);
                        upsert.bind(3, item.generatedAt);
                        upsert.bind(4, item.targetAt);
                        upsert.bind(5, item.horizonHour);
                        upsert.bind(6, item.predictedEnergyMwh);
                        upsert.bind(7, item.predictedFreeCount);
                        upsert.bind(8, item.isPeak ? 1 : 0);
                        upsert.bind(9, item.stale ? 1 : 0);
                        upsert.execute();
                        upsert.reset();
                    }
                });
        });
}

std::vector<LoadPrediction>
PostgresRepository::predictions(const std::optional<std::int64_t> stationId,
                                const std::optional<int> horizonHour, const std::int64_t fromAt)
{
    return useDatabase(
        this, config_,
        [&](QSqlDatabase* database)
        {
            Statement query(
                database, "SELECT station_id,horizon_hour,model_version_no,generated_at,target_at,"
                          "predicted_energy_mwh,predicted_free_count,is_peak,stale "
                          "FROM load_prediction WHERE target_at>=? AND "
                          "(CAST(? AS BIGINT) IS NULL OR station_id=?) AND "
                          "(CAST(? AS INTEGER) IS NULL OR horizon_hour=?) "
                          "AND NOT EXISTS (SELECT 1 FROM load_prediction newer WHERE "
                          "newer.station_id=load_prediction.station_id AND "
                          "newer.target_at=load_prediction.target_at AND "
                          "(newer.generated_at>load_prediction.generated_at OR "
                          "(newer.generated_at=load_prediction.generated_at AND "
                          "newer.model_version_no>load_prediction.model_version_no))) "
                          "ORDER BY target_at,station_id,generated_at DESC");
            query.bind(1, fromAt);
            if (stationId)
            {
                query.bind(2, *stationId);
                query.bind(3, *stationId);
            }
            else
            {
                query.bindNull(2);
                query.bindNull(3);
            }
            if (horizonHour)
            {
                query.bind(4, *horizonHour);
                query.bind(5, *horizonHour);
            }
            else
            {
                query.bindNull(4);
                query.bindNull(5);
            }
            std::vector<LoadPrediction> values;
            while (query.row())
            {
                values.push_back(
                    LoadPrediction{query.integer(0), static_cast<int>(query.integer(1)),
                                   query.text(2), query.integer(3), query.integer(4),
                                   query.integer(5), static_cast<int>(query.integer(6)),
                                   query.integer(7) != 0, query.integer(8) != 0});
            }
            return values;
        });
}

void PostgresRepository::markPredictionsStale()
{
    useDatabase(this, config_,
                [&](QSqlDatabase* database)
                {
                    Statement update(database, "UPDATE load_prediction SET stale=1 WHERE stale=0");
                    update.execute();
                });
}

void PostgresRepository::cleanupAnalytics(const std::int64_t now)
{
    constexpr std::int64_t day = 24 * 3600;
    useDatabase(
        this, config_,
        [&](QSqlDatabase* database)
        {
            Statement predictions(database, "DELETE FROM load_prediction WHERE target_at<?");
            predictions.bind(1, now - 90 * day);
            predictions.execute();
            Statement metrics(database, "DELETE FROM station_hourly_metric WHERE bucket_at<?");
            metrics.bind(1, now - 365 * day);
            metrics.execute();
            // Keep metadata for models still referenced by retained predictions.
            Statement models(database,
                             "DELETE FROM model_version WHERE created_at<? AND version_no NOT IN "
                             "(SELECT DISTINCT model_version_no FROM load_prediction)");
            models.bind(1, now - 30 * day);
            models.execute();
        });
}

// 备份保留（NFR-R-03）：每天最新一份留 7 天、每周最新一份留 4 周，
// 失败备份留 1 周诊断；文件删除前必须通过目录+文件名双重校验。
void PostgresRepository::pruneBackups(const std::int64_t now)
{
    // NFR-R-03: keep the newest backup of each of the last seven days plus the
    // newest of each of the last four weeks; failed diagnostics age out after a
    // week. Files are removed through the same restricted directory check used
    // by verification so a corrupted record cannot delete arbitrary paths.
    const auto records = backups();
    constexpr std::int64_t day = 24 * 3600;
    if (config_.backupDirectory.empty())
        return;
    const std::filesystem::path allowedDirectory =
        std::filesystem::weakly_canonical(std::filesystem::path(config_.backupDirectory));
    std::set<std::int64_t> keptDays;
    std::set<std::int64_t> keptWeeks;
    for (const auto& record : records)
    {
        bool keep = false;
        if (record.status == "SUCCEEDED")
        {
            const std::int64_t dayBucket = record.createdAt / day;
            const std::int64_t weekBucket = record.createdAt / (7 * day);
            if (keptDays.count(dayBucket) == 0 && keptDays.size() < 7)
            {
                keptDays.insert(dayBucket);
                keptWeeks.insert(weekBucket);
                keep = true;
            }
            else if (keptWeeks.count(weekBucket) == 0 && keptWeeks.size() < 4)
            {
                keptWeeks.insert(weekBucket);
                keep = true;
            }
        }
        else if (record.createdAt >= now - 7 * day)
        {
            keep = true;
        }
        if (keep)
            continue;
        if (!record.storagePath.empty() &&
            record.backupNo.find_first_not_of(
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_"
                "-") == std::string::npos)
        {
            std::error_code pathError;
            const std::filesystem::path candidate = std::filesystem::weakly_canonical(
                std::filesystem::path(record.storagePath), pathError);
            if (!pathError && candidate.parent_path() == allowedDirectory &&
                candidate.filename() == record.backupNo + ".dump")
            {
                std::error_code removalError;
                std::filesystem::remove(candidate, removalError);
                if (removalError)
                    continue; // Keep metadata so a failed deletion can be retried.
            }
            else
                continue; // Never forget an artifact that failed the path safety check.
        }
        else if (!record.storagePath.empty())
            continue;
        useDatabase(this, config_,
                    [&](QSqlDatabase* database)
                    {
                        Statement remove(database, "DELETE FROM backup_record WHERE backup_no=?");
                        remove.bind(1, record.backupNo);
                        remove.execute();
                    });
    }
}

// UC-A-09 管理员账号管理（中文导读）：账号列表、创建、停用/启用与改密。
// 每个变更都递增资源 version、并在同一事务写审计事件；除并发 CAS 失败外可安全重试。
// UC-A-09 管理员账号管理: newest-first listing plus create / status /
// password mutations. Every mutation bumps the resource version, writes its
// audit event in the same transaction and is idempotent under retry except
// that a concurrent write wins the compare-and-swap.

} // namespace ncs::infrastructure::postgres
