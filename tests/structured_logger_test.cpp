#include "infrastructure/files/structured_logger.h"
#include "server/middleware/crow_log_handler.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using ncs::infrastructure::files::LogLevel;
using ncs::infrastructure::files::RequestLogScope;
using ncs::infrastructure::files::StructuredLogger;

std::string utf8Path(const QString &path)
{
    const auto bytes = path.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

class TestRunner final {
public:
    void check(const bool condition, const char *message)
    {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            ++failures_;
        }
    }

    int result() const
    {
        return failures_ == 0 ? 0 : 1;
    }

private:
    int failures_ = 0;
};

} // namespace

int main()
{
    TestRunner tests;
    const std::string sanitizedSample = ncs::infrastructure::files::sanitizeSensitiveData(
        "SELECT secret FROM account; failed at /opt/ncs/private.db and C:\\NCS\\secret.db");
    tests.check(
        sanitizedSample.find("SELECT") == std::string::npos
            && sanitizedSample.find("/opt/") == std::string::npos
            && sanitizedSample.find("C:\\") == std::string::npos,
        "SQL and internal paths are filtered at the final log sink");
    QTemporaryDir temporaryDirectory;
    tests.check(temporaryDirectory.isValid(), "temporary log directory is available");
    const QDir directory(temporaryDirectory.path());

    QFile oldLog(directory.filePath(QStringLiteral("ncs_20000101.log")));
    tests.check(oldLog.open(QIODevice::WriteOnly), "old log can be created");
    oldLog.write("old\n");
    oldLog.close();
    QFile unrelated(directory.filePath(QStringLiteral("ncs_invalid.log")));
    tests.check(unrelated.open(QIODevice::WriteOnly), "unrelated log can be created");
    unrelated.write("keep\n");
    unrelated.close();

    {
        StructuredLogger logger({
            utf8Path(temporaryDirectory.path()),
            LogLevel::Info,
            30,
            false,
        });
        tests.check(!oldLog.exists(), "expired dated log is removed");
        tests.check(unrelated.exists(), "unrecognized log name is preserved");

        logger.log(LogLevel::Debug, "test", "filtered debug event");
        {
            RequestLogScope requestScope("request-123");
            logger.log(LogLevel::Info, "test.module", "message with \"quotes\" and\nline");
        }

        ncs::server::middleware::CrowLogHandler crowHandler(logger);
        crowHandler.log(
            "GET /api/v1/test?token=secret Authorization: Bearer secret-token",
            crow::LogLevel::Info);
        logger.log(
            LogLevel::Warning,
            "sensitive",
            R"({"phone":"13800138000","verificationCode":"123456","balanceCent":900,"orderNo":"ORDER-SECRET"})");

        std::vector<std::thread> writers;
        for (int threadIndex = 0; threadIndex < 4; ++threadIndex) {
            writers.emplace_back([threadIndex, &logger] {
                RequestLogScope requestScope("thread-" + std::to_string(threadIndex));
                for (int eventIndex = 0; eventIndex < 10; ++eventIndex) {
                    logger.log(LogLevel::Warning, "concurrency", "threaded event");
                }
            });
        }
        for (auto &writer : writers) {
            writer.join();
        }
    }

    const QString logName = QStringLiteral("ncs_%1.log").arg(
        QDateTime::currentDateTimeUtc().date().toString(QStringLiteral("yyyyMMdd")));
    QFile logFile(directory.filePath(logName));
    tests.check(logFile.open(QIODevice::ReadOnly | QIODevice::Text), "daily log can be read");

    int eventCount = 0;
    bool foundScopedEvent = false;
    bool foundSanitizedCrowEvent = false;
    bool foundSanitizedBusinessEvent = false;
    while (!logFile.atEnd()) {
        const QByteArray line = logFile.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(line, &error);
        tests.check(error.error == QJsonParseError::NoError, "each log line is valid JSON");
        tests.check(document.isObject(), "each log line is a JSON object");
        const QJsonObject event = document.object();
        tests.check(event.contains(QStringLiteral("timestamp")), "event has timestamp");
        tests.check(event.contains(QStringLiteral("level")), "event has level");
        tests.check(event.contains(QStringLiteral("module")), "event has module");
        tests.check(event.contains(QStringLiteral("requestId")), "event has request ID");
        tests.check(event.contains(QStringLiteral("message")), "event has message");

        const QString requestId = event.value(QStringLiteral("requestId")).toString();
        const QString message = event.value(QStringLiteral("message")).toString();
        if (requestId == QStringLiteral("request-123")) {
            foundScopedEvent = true;
        }
        if (event.value(QStringLiteral("module")).toString() == QStringLiteral("crow")) {
            foundSanitizedCrowEvent = message.contains(QStringLiteral("?<redacted>"))
                && message.contains(QStringLiteral("Bearer <redacted>"))
                && !message.contains(QStringLiteral("secret-token"));
        }
        if (event.value(QStringLiteral("module")).toString() == QStringLiteral("sensitive")) {
            foundSanitizedBusinessEvent = message.count(QStringLiteral("<redacted>")) == 4
                && !message.contains(QStringLiteral("13800138000"))
                && !message.contains(QStringLiteral("123456"))
                && !message.contains(QStringLiteral("ORDER-SECRET"));
        }
        ++eventCount;
    }

    tests.check(eventCount == 43, "level filtering and concurrent writes preserve event count");
    tests.check(foundScopedEvent, "request scope supplies request ID");
    tests.check(foundSanitizedCrowEvent, "Crow query and bearer data are redacted");
    tests.check(foundSanitizedBusinessEvent, "business-sensitive log fields are redacted");
    return tests.result();
}
