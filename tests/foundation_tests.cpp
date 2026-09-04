#include "config/application_config.h"
#include "logging/application_logger.h"
#include "ncs/core/error.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>

namespace
{

int failures = 0;

void expect(const bool condition, const char* message)
{
    if (!condition)
    {
        qCritical() << "FAILED:" << message;
        ++failures;
    }
}

QString writeConfig(QTemporaryDir& temporaryDirectory, const QString& contents)
{
    const QString path = QDir(temporaryDirectory.path()).filePath(QStringLiteral("test.env"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        return {};
    }
    QTextStream stream(&file);
    stream << contents;
    return path;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("NCS"));
    QCoreApplication::setApplicationName(QStringLiteral("ncs_foundation_tests"));

    expect(ncs::core::errorCodeName(ncs::core::ErrorCode::QuoteExpired) ==
               QStringLiteral("QUOTE_EXPIRED"),
           "shared error code name must match the API contract");
    expect(static_cast<int>(ncs::core::ErrorCode::Unauthorized) == 401,
           "shared unauthorized code must remain stable");

    QTemporaryDir temporaryDirectory;
    expect(temporaryDirectory.isValid(), "temporary directory must be available");

    const QString configPath = writeConfig(
        temporaryDirectory,
        QStringLiteral("NCS_ENV=test\nNCS_SERVER_HOST=127.0.0.1\nNCS_SERVER_PORT=18443\n"
                       "NCS_DATABASE_PATH=%1/data/test.db\nNCS_LOG_DIR=%1/logs\n"
                       "NCS_ALLOW_INSECURE_HTTP=true\n")
            .arg(temporaryDirectory.path()));
    expect(!configPath.isEmpty(), "test configuration must be created");

    auto config = ncs::infrastructure::ApplicationConfig::load(configPath);
    expect(config.hasValue(), "valid test configuration must load");
    if (config)
    {
        expect(config.value().environment() == QStringLiteral("test"),
               "environment value must be preserved");
        expect(config.value().serverPort() == 18443, "server port must be parsed");

        auto logger = ncs::infrastructure::ApplicationLogger::initialize(
            config.value().logDirectory(), QStringLiteral("foundation-test"));
        expect(logger.hasValue(), "logger must initialize in an isolated directory");
        if (logger)
        {
            ncs::infrastructure::ApplicationLogger::setRequestId(QStringLiteral("test-request"));
            qInfo() << "foundation logger smoke token=must-not-be-written";
            ncs::infrastructure::ApplicationLogger::shutdown();

            const QString logPath =
                QDir(config.value().logDirectory())
                    .filePath(QStringLiteral("ncs_%1.log")
                                  .arg(QDate::currentDate().toString(QStringLiteral("yyyyMMdd"))));
            QFile log(logPath);
            expect(log.open(QIODevice::ReadOnly | QIODevice::Text), "log file must be readable");
            const QString logContents = QString::fromUtf8(log.readAll());
            expect(logContents.contains(QStringLiteral("test-request")),
                   "log line must contain the request ID");
            expect(logContents.contains(QStringLiteral("token=[REDACTED]")),
                   "sensitive log field must be redacted");
            expect(!logContents.contains(QStringLiteral("must-not-be-written")),
                   "sensitive log value must not be persisted");
        }
    }

    if (failures == 0)
    {
        qInfo() << "All foundation tests passed.";
    }
    return failures == 0 ? 0 : 1;
}
