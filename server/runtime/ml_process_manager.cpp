#include "server/runtime/ml_process_manager.h"

#include <QProcess>
#include <QStringList>

#include <chrono>
#include <future>

namespace ncs::server::runtime
{
namespace
{

std::string horizons(const std::vector<int>& values)
{
    std::string result;
    for (const int value : values)
    {
        if (!result.empty())
            result += ',';
        result += std::to_string(value);
    }
    return result;
}

std::int64_t unixNow()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

MlProcessManager::MlProcessManager(MlProcessOptions options,
                                   core::application::SessionManager& sessions,
                                   core::application::AdminRepository& admin,
                                   core::application::AnalyticsRepository& analytics,
                                   core::application::ModelArtifactStore* artifacts)
    : options_(std::move(options)), sessions_(sessions), admin_(admin), analytics_(analytics),
      artifacts_(artifacts)
{
}

// 析构：置位停止标记并 join 全部 worker 线程，把仍在 PENDING/RUNNING 的任务标记为 FAILED
// （“服务关闭，ML 任务已停止”）并刷新预测数据，保证关闭不遗留悬挂任务。
MlProcessManager::~MlProcessManager()
{
    std::vector<std::pair<std::string, std::shared_ptr<Worker>>> workers;
    {
        std::lock_guard lock(mutex_);
        for (auto& [taskNo, worker] : workers_)
        {
            (void)taskNo;
            worker->stopRequested = true;
            workers.emplace_back(taskNo, worker);
        }
        workers_.clear();
    }
    for (const auto& [taskNo, worker] : workers)
    {
        if (worker->thread.joinable())
            worker->thread.join();
        try
        {
            auto task = admin_.mlTask(taskNo);
            if (task && (task->status == "PENDING" || task->status == "RUNNING"))
            {
                task->status = "FAILED";
                task->finishedAt = unixNow();
                task->errorSummary = "服务关闭，ML 任务已停止";
                if (admin_.tryFinishMlTask(*task, false))
                    analytics_.markPredictionsStale();
            }
        }
        catch (...)
        {
        }
    }
}

// 启动 ML 子进程：TRAIN 任务令牌寿命 10 分钟、PREDICT 2 分钟，先签发受限的 MlTask 令牌并
// 经 stdin 传递（不出现在命令行与日志）；训练产物写入 .staging-<taskNo> 暂存路径；worker
// 线程轮询等待进程退出，异常退出时把任务标记为 FAILED 并吊销会话。
bool MlProcessManager::start(const core::application::MlTask& task)
{
    reapFinished();
    const auto lifetime =
        task.taskType == "TRAIN" ? std::chrono::minutes(10) : std::chrono::minutes(2);
    auto latestModel =
        task.taskType == "PREDICT" ? analytics_.latestQualifiedModel() : std::nullopt;
    if (latestModel && artifacts_ &&
        !artifacts_->verifyArtifact(latestModel->artifactPath, latestModel->artifactChecksum))
    {
        // A qualified model exists but its artifact is missing or corrupt.
        // Falling back to BASELINE silently would present degraded numbers as
        // valid and let BASELINE rows supersede the still-correct MODEL
        // forecasts on the dashboard, so fail the task loudly instead.
        auto persisted = admin_.mlTask(task.taskNo);
        if (persisted && (persisted->status == "PENDING" || persisted->status == "RUNNING"))
        {
            persisted->status = "FAILED";
            persisted->finishedAt = unixNow();
            persisted->errorSummary = "合格模型产物缺失或校验失败";
            if (admin_.tryFinishMlTask(*persisted, false))
                analytics_.markPredictionsStale();
        }
        return false;
    }
    const std::string modelVersion = latestModel ? latestModel->versionNo : "BASELINE";
    const std::string predictionModelPath =
        latestModel ? latestModel->artifactPath : options_.modelPath;
    auto token = sessions_.issue("ml:" + task.taskNo, "worker:" + task.taskNo,
                                 core::application::TokenKind::MlTask,
                                 {core::application::Role::MlWorker,
                                  task.taskType == "TRAIN" ? core::application::Role::MlTrainer
                                                           : core::application::Role::MlPredictor},
                                 std::chrono::system_clock::now(), lifetime);
    if (!token)
        return false;

    auto worker = std::make_shared<Worker>();
    auto started = std::make_shared<std::promise<bool>>();
    auto startedFuture = started->get_future();
    const std::int64_t sessionId = token->context.sessionId;
    const std::string rawToken = std::move(token->accessToken);
    const MlProcessOptions options = options_;
    worker->thread = std::thread(
        [this, worker, started, task, sessionId, rawToken, options, modelVersion,
         predictionModelPath]
        {
            bool promiseCompleted = false;
            try
            {
                QProcess process;
                process.setStandardOutputFile(QProcess::nullDevice());
                process.setStandardErrorFile(QProcess::nullDevice());
                const std::string modelPath = task.taskType == "TRAIN"
                                                  ? options.modelPath + ".staging-" + task.taskNo
                                                  : predictionModelPath;
                QStringList arguments{
                    QString::fromStdString(options.workerScript),
                    QStringLiteral("--mode"),
                    QString::fromStdString(task.taskType == "TRAIN" ? "train" : "predict"),
                    QStringLiteral("--task-no"),
                    QString::fromStdString(task.taskNo),
                    QStringLiteral("--base-url"),
                    QString::fromStdString(options.baseUrl),
                    QStringLiteral("--ca-file"),
                    QString::fromStdString(options.caCertificatePath),
                    QStringLiteral("--model-path"),
                    QString::fromStdString(modelPath),
                    QStringLiteral("--model-version"),
                    QString::fromStdString(modelVersion),
                    QStringLiteral("--horizons"),
                    QString::fromStdString(horizons(task.horizonHours)),
                    QStringLiteral("--token-stdin")};
                process.start(QString::fromStdString(options.pythonExecutable), arguments,
                              QIODevice::ReadWrite);
                const bool didStart = process.waitForStarted(5000);
                started->set_value(didStart);
                promiseCompleted = true;
                if (!didStart)
                    throw std::runtime_error("ML worker failed to start");
                process.write(QByteArray::fromStdString(rawToken + "\n"));
                if (!process.waitForBytesWritten(5000))
                    throw std::runtime_error("ML worker token delivery failed");
                process.closeWriteChannel();
                while (!process.waitForFinished(250))
                {
                    if (worker->stopRequested)
                    {
                        process.terminate();
                        if (!process.waitForFinished(3000))
                        {
                            process.kill();
                            process.waitForFinished(3000);
                        }
                        break;
                    }
                }
            }
            catch (...)
            {
                if (!promiseCompleted)
                    started->set_value(false);
            }

            sessions_.revoke(sessionId);
            try
            {
                auto persisted = admin_.mlTask(task.taskNo);
                if (persisted &&
                    (persisted->status == "PENDING" || persisted->status == "RUNNING") &&
                    !worker->stopRequested)
                {
                    persisted->status = "FAILED";
                    persisted->finishedAt = unixNow();
                    persisted->errorSummary = "ML 子进程异常退出";
                    if (admin_.tryFinishMlTask(*persisted, false))
                        analytics_.markPredictionsStale();
                }
            }
            catch (...)
            {
            }
            worker->finished = true;
        });

    if (!startedFuture.get())
    {
        if (worker->thread.joinable())
            worker->thread.join();
        sessions_.revoke(sessionId);
        return false;
    }
    std::lock_guard lock(mutex_);
    workers_.emplace(task.taskNo, std::move(worker));
    return true;
}

// 请求停止指定任务：仅置位 stopRequested，由 worker 线程在轮询间隔内先 terminate、超时后
// kill 子进程，避免跨线程直接操作 QProcess。
void MlProcessManager::stop(const std::string_view taskNo)
{
    std::shared_ptr<Worker> worker;
    {
        std::lock_guard lock(mutex_);
        const auto found = workers_.find(std::string(taskNo));
        if (found == workers_.end())
            return;
        worker = found->second;
    }
    worker->stopRequested = true;
}

// 收割已结束的 worker：锁内摘除 finished 条目、锁外 join 线程，完成任务互斥登记表的清理。
void MlProcessManager::reapFinished()
{
    std::vector<std::shared_ptr<Worker>> finished;
    {
        std::lock_guard lock(mutex_);
        for (auto iterator = workers_.begin(); iterator != workers_.end();)
        {
            if (!iterator->second->finished)
            {
                ++iterator;
                continue;
            }
            finished.push_back(iterator->second);
            iterator = workers_.erase(iterator);
        }
    }
    for (const auto& worker : finished)
    {
        if (worker->thread.joinable())
            worker->thread.join();
    }
}

} // namespace ncs::server::runtime
