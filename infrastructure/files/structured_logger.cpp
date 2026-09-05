#include "infrastructure/files/structured_logger.h"

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QString>

#include <cstdio>
#include <stdexcept>
#include <utility>

namespace ncs::infrastructure::files
{
namespace
{

thread_local std::string requestIdContext;

QString fromUtf8(const std::string_view value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

int levelRank(const LogLevel level)
{
    return static_cast<int>(level);
}

} // namespace

std::string_view logLevelName(const LogLevel level)
{
    switch (level)
    {
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warning:
        return "WARNING";
    case LogLevel::Error:
        return "ERROR";
    case LogLevel::Critical:
        return "CRITICAL";
    }
    return "UNKNOWN";
}

std::string sanitizeSensitiveData(const std::string_view message)
{
    QString sanitized = fromUtf8(message);
    sanitized.replace(QRegularExpression(QStringLiteral("(?i)(Bearer\\s+)[A-Za-z0-9._~-]+")),
                      QStringLiteral("\\1<redacted>"));
    sanitized.replace(QRegularExpression(QStringLiteral(
                          "(?i)([?&](?:token|accessToken|code|phone|password)\\s*=)[^&\\s]+")),
                      QStringLiteral("\\1<redacted>"));
    sanitized.replace(
        QRegularExpression(QStringLiteral(
            "(?i)([\\\"](?:phone|mobile|token|accessToken|password|verificationCode|code|balance|"
            "balanceCent|wallet|orderNo)[\\\"]\\s*:\\s*)[\\\"]?[^,}\\s\\\"]+[\\\"]?")),
        QStringLiteral("\\1\"<redacted>\""));
    sanitized.replace(QRegularExpression(QStringLiteral("(?<!\\d)\\d{11,}(?!\\d)")),
                      QStringLiteral("<redacted-number>"));
    sanitized.replace(
        QRegularExpression(QStringLiteral(
            "(?i)\\b(?:SELECT|INSERT|UPDATE|DELETE|PRAGMA|CREATE|DROP)\\b[^;\\n]*;?")),
        QStringLiteral("<redacted-sql>"));
    sanitized.replace(
        // A filesystem path never contains '?': stop there so an already
        // redacted query marker like ?<redacted> survives this pass.
        QRegularExpression(QStringLiteral("(?<![A-Za-z0-9:])/(?:[^\\s,;?]+/)*[^\\s,;?]*")),
        QStringLiteral("<redacted-path>"));
    sanitized.replace(QRegularExpression(QStringLiteral("(?i)\\b[A-Z]:\\\\[^\\s,;]+")),
                      QStringLiteral("<redacted-path>"));
    const QByteArray bytes = sanitized.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

RequestLogScope::RequestLogScope(std::string requestId)
    : previousRequestId_(std::move(requestIdContext))
{
    requestIdContext = std::move(requestId);
}

RequestLogScope::~RequestLogScope()
{
    requestIdContext = std::move(previousRequestId_);
}

std::string_view currentRequestId()
{
    return requestIdContext;
}

class StructuredLogger::Impl final
{
  public:
    explicit Impl(Options options)
        : options_(std::move(options)), directory_(fromUtf8(options_.directory))
    {
        if (options_.retentionDays < 1)
        {
            throw std::invalid_argument("log retention must be at least one day");
        }
        if (options_.directory.empty() ||
            (!directory_.exists() && !QDir().mkpath(directory_.absolutePath())))
        {
            throw std::runtime_error("log directory could not be created");
        }
        cleanupExpiredUnlocked();
    }

    void log(const LogLevel level, const std::string_view module, const std::string_view message,
             const std::string_view requestId)
    {
        if (levelRank(level) < levelRank(options_.minimumLevel))
        {
            return;
        }

        const QDateTime now = QDateTime::currentDateTimeUtc();
        QJsonObject event{
            {QStringLiteral("timestamp"), now.toString(Qt::ISODateWithMs)},
            {QStringLiteral("level"), fromUtf8(logLevelName(level))},
            {QStringLiteral("module"), fromUtf8(module.empty() ? "unknown" : module)},
            {QStringLiteral("requestId"), fromUtf8(requestId)},
            {QStringLiteral("message"), fromUtf8(message)},
        };
        QByteArray line = QJsonDocument(event).toJson(QJsonDocument::Compact);
        line.append('\n');

        {
            QMutexLocker locker(&mutex_);
            ensureFileOpen(now.date());
            if (file_.write(line) != line.size() || !file_.flush())
            {
                throw std::runtime_error("structured log write failed");
            }
        }
        // Write to the console outside the mutex: a blocked stderr (for example a
        // full pipe buffer on Windows) must not stall every logging thread and
        // deadlock the server.
        if (options_.consoleEnabled)
        {
            std::fwrite(line.constData(), 1, static_cast<std::size_t>(line.size()), stderr);
            std::fflush(stderr);
        }
    }

    void cleanupExpired()
    {
        QMutexLocker locker(&mutex_);
        cleanupExpiredUnlocked();
    }

  private:
    void ensureFileOpen(const QDate& date)
    {
        if (file_.isOpen() && openDate_ == date)
        {
            return;
        }
        file_.close();
        openDate_ = date;
        file_.setFileName(directory_.filePath(
            QStringLiteral("ncs_%1.log").arg(date.toString(QStringLiteral("yyyyMMdd")))));
        if (!file_.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        {
            throw std::runtime_error("structured log file could not be opened");
        }
#ifdef Q_OS_UNIX
        file_.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
#endif
    }

    void cleanupExpiredUnlocked()
    {
        static const QRegularExpression pattern(QStringLiteral("^ncs_(\\d{8})\\.log$"));
        const QDate cutoff =
            QDateTime::currentDateTimeUtc().date().addDays(-(options_.retentionDays - 1));
        const auto files =
            directory_.entryInfoList({QStringLiteral("ncs_*.log")}, QDir::Files | QDir::NoSymLinks);
        for (const QFileInfo& file : files)
        {
            const auto match = pattern.match(file.fileName());
            if (!match.hasMatch())
            {
                continue;
            }
            const QDate date = QDate::fromString(match.captured(1), QStringLiteral("yyyyMMdd"));
            if (date.isValid() && date < cutoff)
            {
                QFile::remove(file.absoluteFilePath());
            }
        }
    }

    Options options_;
    QDir directory_;
    QMutex mutex_;
    QFile file_;
    QDate openDate_;
};

StructuredLogger::StructuredLogger(Options options)
    : impl_(std::make_unique<Impl>(std::move(options)))
{
}

StructuredLogger::~StructuredLogger() = default;

void StructuredLogger::log(const LogLevel level, const std::string_view module,
                           const std::string_view message, const std::string_view requestId)
{
    const std::string_view effectiveRequestId = requestId.empty() ? currentRequestId() : requestId;
    const std::string sanitized = sanitizeSensitiveData(message);
    impl_->log(level, module, sanitized, effectiveRequestId);
}

void StructuredLogger::cleanupExpired()
{
    impl_->cleanupExpired();
}

} // namespace ncs::infrastructure::files
