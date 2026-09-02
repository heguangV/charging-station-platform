#include "logging/application_logger.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QRegularExpression>
#include <QTextStream>
#include <QtGlobal>

namespace ncs::infrastructure
{
namespace
{

QMutex loggerMutex;
QFile logFile;
QString loggerModule;
thread_local QString currentRequestId;
QtMessageHandler previousHandler = nullptr;

QString levelName(const QtMsgType type)
{
    switch (type)
    {
    case QtDebugMsg:
        return QStringLiteral("DEBUG");
    case QtInfoMsg:
        return QStringLiteral("INFO");
    case QtWarningMsg:
        return QStringLiteral("WARN");
    case QtCriticalMsg:
        return QStringLiteral("ERROR");
    case QtFatalMsg:
        return QStringLiteral("FATAL");
    }
    return QStringLiteral("UNKNOWN");
}

QString redact(QString message)
{
    static const QRegularExpression sensitive(QStringLiteral(
        "(?i)(authorization|token|password|secret|code|phone)\\s*[:=]\\s*([^\\s,;]+)"));
    message.replace(sensitive, QStringLiteral("\\1=[REDACTED]"));
    return message;
}

void messageHandler(const QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    const QString timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    const QString category = context.category == nullptr ? QStringLiteral("default")
                                                         : QString::fromUtf8(context.category);
    const QString requestId = currentRequestId.isEmpty() ? QStringLiteral("-") : currentRequestId;
    const QString line =
        QStringLiteral("%1 [%2] [%3] [%4] [%5] %6\n")
            .arg(timestamp, levelName(type), loggerModule, category, requestId, redact(message));

    QMutexLocker locker(&loggerMutex);
    if (logFile.isOpen())
    {
        logFile.write(line.toUtf8());
        logFile.flush();
    }
    if (previousHandler != nullptr)
    {
        previousHandler(type, context, message);
    }
}

} // namespace

ncs::core::Result<void> ApplicationLogger::initialize(const QString& logDirectory,
                                                      const QString& module)
{
    QMutexLocker locker(&loggerMutex);
    if (logFile.isOpen())
    {
        return ncs::core::Result<void>::success();
    }

    QDir directory;
    if (!directory.mkpath(logDirectory))
    {
        return ncs::core::Result<void>::failure({ncs::core::ErrorCode::InternalError,
                                                 QStringLiteral("cannot create log directory"),
                                                 QStringLiteral("无法创建日志目录"),
                                                 {}});
    }

    const QDateTime retentionCutoff = QDateTime::currentDateTimeUtc().addDays(-30);
    const QFileInfoList oldLogs =
        QDir(logDirectory)
            .entryInfoList({QStringLiteral("ncs_????????.log")}, QDir::Files | QDir::NoSymLinks);
    for (const QFileInfo& oldLog : oldLogs)
    {
        if (oldLog.lastModified().toUTC() < retentionCutoff)
        {
            QFile::remove(oldLog.absoluteFilePath());
        }
    }

    const QString fileName =
        QStringLiteral("ncs_%1.log").arg(QDate::currentDate().toString(QStringLiteral("yyyyMMdd")));
    logFile.setFileName(QDir(logDirectory).filePath(fileName));
    if (!logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
    {
        return ncs::core::Result<void>::failure({ncs::core::ErrorCode::InternalError,
                                                 QStringLiteral("cannot open log file"),
                                                 QStringLiteral("无法打开日志文件"),
                                                 {}});
    }

    loggerModule = module;
    previousHandler = qInstallMessageHandler(messageHandler);
    return ncs::core::Result<void>::success();
}

void ApplicationLogger::setRequestId(const QString& requestId)
{
    currentRequestId = requestId.left(128);
}

void ApplicationLogger::shutdown()
{
    qInstallMessageHandler(previousHandler);
    QMutexLocker locker(&loggerMutex);
    logFile.close();
    loggerModule.clear();
    currentRequestId.clear();
    previousHandler = nullptr;
}

} // namespace ncs::infrastructure
