// 管理端运营应用服务（实施指南
// §6.3/6.4）：充电流程监管与强制释放、营收/充电桩状态统计、审计事件查询、备份编排记录与 ML
// 任务调度。 依赖 AdminRepository、ChargingRepository、ChargeFlowService 与
// BusinessNumbers；AnalyticsRepository、MlTaskLauncher 为可选端口（仪表盘数据与 ML 子进程启动）。
// 约束：金额用整数分、电量用毫瓦时；营收分桶仅支持 day/hour；ML 任务训练 600 秒、预测 120
// 秒超时，同类型运行中任务去重。

#pragma once

#include "core/application/admin_repository.h"
#include "core/application/analytics_service.h"
#include "core/application/business_numbers.h"
#include "core/application/charge_flow_service.h"
#include "core/application/service_result.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ncs::core::application
{

struct RevenueBucket
{
    std::int64_t bucketStart = 0;
    std::int64_t amountCent = 0;
    std::int64_t energyMwh = 0;
    int orderCount = 0;
};

struct RevenueStats
{
    std::vector<RevenueBucket> items;
    std::int64_t totalAmountCent = 0;
    std::int64_t totalEnergyMwh = 0;
    int totalOrderCount = 0;
};

struct ChargerStatusStats
{
    int idleCount = 0;
    int occupiedCount = 0;
    int faultyCount = 0;
    int restartingCount = 0;
    int disabledCount = 0;
    int operationalCount = 0;
    int totalCount = 0;
    int healthPercent = 0;
};

struct PredictionView
{
    std::int64_t stationId = 0;
    int horizonHour = 1;
    std::string modelVersion;
    std::int64_t generatedAt = 0;
    std::int64_t targetAt = 0;
    std::int64_t predictedEnergyMwh = 0;
    int predictedIdleCount = 0;
    bool peakFlag = false;
    bool staleFlag = false;
};

class MlTaskLauncher
{
  public:
    virtual ~MlTaskLauncher() = default;
    virtual bool start(const MlTask& task) = 0;
    virtual void stop(std::string_view taskNo) = 0;
};

// Section 6.3/6.4 admin operations: flow oversight, force release, statistics,
// audit queries, backup orchestration records and ML task bookkeeping.
class AdminOpsService final
{
  public:
    AdminOpsService(AdminRepository& repository, ChargingRepository& charging,
                    ChargeFlowService& flows, BusinessNumbers& numbers,
                    AnalyticsRepository* analytics = nullptr, MlTaskLauncher* launcher = nullptr)
        : repository_(repository), charging_(charging), flows_(flows), numbers_(numbers),
          analytics_(analytics), launcher_(launcher)
    {
    }

    AdminFlowPage flows(const AdminFlowQuery& query);
    // 强制释放充电流程：校验审计原因后委托 ChargeFlowService::adminForceRelease
    // 完成状态机变更，成功追加 FORCE_RELEASE 审计事件。
    ServiceResult<FlowView> forceRelease(std::int64_t actorAdminId, const std::string& flowNo,
                                         const std::string& reason, int nextChargerStatus,
                                         std::int64_t flowVersion,
                                         std::chrono::system_clock::time_point now);

    // ---- 统计：营收按 day/hour 分桶聚合、充电桩健康度概览 ----
    // 营收统计：已结算订单按 day/hour 分桶聚合金额（分）与电量（毫瓦时）；时间缺省取最近 30
    // 天，跨度不得超过 90 天，桶名非法返回 ValidationFailed。
    ServiceResult<RevenueStats>
    revenueStats(std::int64_t fromAt, std::int64_t toAt, std::optional<std::int64_t> stationId,
                 const std::string& bucket,
                 std::chrono::system_clock::time_point now = std::chrono::system_clock::now());
    ChargerStatusStats chargerStatusStats(std::optional<std::int64_t> stationId);

    std::vector<AuditEvent> auditEvents(const AuditEventQuery& query);

    // ---- 备份编排：登记 BackupRecord 并触发快照，实际创建/校验由仓储完成 ----
    // 创建备份：先登记 PENDING 记录再触发快照；快照失败置 FAILED、记 BACKUP_FAILED 审计并返回
    // TransactionFailed，成功置 SUCCEEDED。
    ServiceResult<BackupRecord> createBackup(std::int64_t actorAdminId,
                                             std::chrono::system_clock::time_point now);
    std::vector<BackupRecord> backups();
    // 校验备份快照：置 RUNNING 后执行校验，结果写回 SUCCEEDED/FAILED 并记审计；备份不存在返回
    // NotFound，校验失败返回 TransactionFailed。
    ServiceResult<BackupRecord> verifyBackup(std::int64_t actorAdminId, const std::string& backupNo,
                                             std::chrono::system_clock::time_point now);

    // ---- ML 任务：启动/查询/超时收尾（completeTimedOutMlTasks）与预测结果读取 ----
    // 启动 ML
    // 任务（TRAIN/PREDICT）：先收尾超时任务；同类型已有运行中任务则直接返回该任务（幂等去重）；
    // 子进程启动失败将任务置 FAILED、预测标记过期并返回 ExternalServiceUnavailable。
    ServiceResult<MlTask> startMlTask(std::int64_t actorAdminId, const std::string& taskType,
                                      const std::vector<int>& horizonHours,
                                      std::chrono::system_clock::time_point now);
    // 查询 ML 任务；命中超时未收尾的运行中任务时先补记 TIMED_OUT 再返回最新状态，任务不存在返回
    // NotFound。
    ServiceResult<MlTask>
    mlTask(const std::string& taskNo,
           std::chrono::system_clock::time_point now = std::chrono::system_clock::now());
    // UC-M-04: training tasks time out after ten minutes and prediction tasks
    // after two; without this transition the running-task dedup would block the
    // task type forever because no ML subprocess exists yet.
    void completeTimedOutMlTasks(std::chrono::system_clock::time_point now);
    std::vector<PredictionView> predictions(std::optional<std::int64_t> stationId,
                                            std::optional<int> horizonHour,
                                            std::int64_t fromAt = 0);

    static bool validBucket(const std::string& bucket)
    {
        return bucket == "day" || bucket == "hour";
    }

    static constexpr std::int64_t kTrainTimeoutSeconds = 600;
    static constexpr std::int64_t kPredictTimeoutSeconds = 120;

  private:
    void completeTimedOutMlTasksUnlocked(std::chrono::system_clock::time_point now);
    AdminRepository& repository_;
    ChargingRepository& charging_;
    ChargeFlowService& flows_;
    BusinessNumbers& numbers_;
    AnalyticsRepository* analytics_ = nullptr;
    MlTaskLauncher* launcher_ = nullptr;
    std::mutex mlStartMutex_;
};

} // namespace ncs::core::application
