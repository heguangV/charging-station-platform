#include "core/application/analytics_service.h"
#include "core/application/business_numbers.h"
#include "infrastructure/sqlite/sqlite_repository.h"
#include "server/runtime/ml_process_manager.h"

#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <chrono>
#include <iostream>
#include <thread>

int main()
{
    using namespace ncs;
    QTemporaryDir temporary;
    if (!temporary.isValid())
        return 1;
    const QString scriptPath = temporary.path() + "/worker.py";
    QFile script(scriptPath);
    if (!script.open(QIODevice::WriteOnly) ||
        script.write("import sys\nsys.stdin.readline()\nraise SystemExit(7)\n") <= 0)
        return 1;
    script.close();

    QString pythonExecutable = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (pythonExecutable.isEmpty())
        pythonExecutable = QStandardPaths::findExecutable(QStringLiteral("python"));
    if (pythonExecutable.isEmpty())
    {
        std::cerr << "FAIL: Python executable was not found\n";
        return 1;
    }

    infrastructure::sqlite::SqliteRepository repository(
        (temporary.path() + "/runtime.db").toStdString());
    core::application::SessionManager sessions;
    server::runtime::MlProcessManager manager(
        server::runtime::MlProcessOptions{pythonExecutable.toStdString(), scriptPath.toStdString(),
                                          "https://127.0.0.1:1", "unused.pem",
                                          (temporary.path() + "/model.pkl").toStdString()},
        sessions, repository, repository);
    core::application::MlTask task;
    task.taskNo = "MLPROCESS0001";
    task.taskType = "PREDICT";
    task.status = "RUNNING";
    task.createdAt = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    repository.addMlTask(task);
    if (!manager.start(task))
    {
        std::cerr << "FAIL: worker process did not start\n";
        return 1;
    }
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        const auto value = repository.mlTask(task.taskNo);
        if (value && value->status == "FAILED")
            return value->errorSummary == "ML 子进程异常退出" ? 0 : 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::cerr << "FAIL: abnormal worker exit was not persisted\n";
    return 1;
}
