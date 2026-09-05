#include "infrastructure/files/structured_logger.h"
#include "server/middleware/global_exception_handler.h"

#include <crow.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using ncs::infrastructure::files::LogLevel;
using ncs::infrastructure::files::RequestLogScope;
using ncs::infrastructure::files::StructuredLogger;

constexpr auto requestId = "exception-test-request";
constexpr auto sensitiveException =
    "SELECT token FROM sessions; /srv/private/database.sqlite bearer-secret";

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

bool containsSensitiveText(const QByteArray &value)
{
    return value.contains("SELECT")
        || value.contains("/srv/private")
        || value.contains("database.sqlite")
        || value.contains("bearer-secret");
}

} // namespace

int main()
{
    TestRunner tests;
    QTemporaryDir temporaryDirectory;
    tests.check(temporaryDirectory.isValid(), "temporary log directory is available");

    crow::response response;
    {
        StructuredLogger logger({
            utf8Path(temporaryDirectory.path()),
            LogLevel::Info,
            30,
            false,
        });
        ncs::server::ServerApp application;
        ncs::server::middleware::installGlobalExceptionHandler(application, logger);
        application.route_dynamic("/throws")([](const crow::request &, crow::response &partial) {
            partial.code = 202;
            partial.body = sensitiveException;
            partial.set_header("X-Internal-Debug", sensitiveException);
            throw std::runtime_error(sensitiveException);
        });
        application.validate();

        crow::request request;
        request.method = crow::HTTPMethod::GET;
        request.url = "/throws";
        RequestLogScope requestScope(requestId);
        application.handle_full(request, response);
    }

    tests.check(response.code == 500, "unhandled exception maps to HTTP 500");
    tests.check(
        response.get_header_value("Content-Type") == "application/json; charset=utf-8",
        "error response declares UTF-8 JSON");
    tests.check(
        response.get_header_value("Cache-Control") == "no-store",
        "error response disables caching");
    tests.check(
        response.get_header_value("X-Internal-Debug").empty(),
        "error response clears partial internal headers");

    const QByteArray body = QByteArray::fromStdString(response.body);
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    tests.check(parseError.error == QJsonParseError::NoError, "error body is valid JSON");
    tests.check(document.isObject(), "error body is a JSON object");
    const QJsonObject object = document.object();
    tests.check(object.size() == 6, "error body contains only contract fields");
    tests.check(
        object.contains(QStringLiteral("success"))
            && object.contains(QStringLiteral("code"))
            && object.contains(QStringLiteral("message"))
            && object.contains(QStringLiteral("userMessage"))
            && object.contains(QStringLiteral("requestId"))
            && object.contains(QStringLiteral("data")),
        "all failure envelope fields are present");
    tests.check(!object.value(QStringLiteral("success")).toBool(true), "success is false");
    tests.check(object.value(QStringLiteral("code")).toInt() == 13, "code is INTERNAL_ERROR");
    tests.check(
        object.value(QStringLiteral("message")).toString() == QStringLiteral("internal error"),
        "diagnostic message is generic");
    tests.check(
        !object.value(QStringLiteral("userMessage")).toString().isEmpty(),
        "user message is displayable");
    tests.check(
        object.value(QStringLiteral("requestId")).toString() == QString::fromUtf8(requestId),
        "request ID follows the current log scope");
    tests.check(object.value(QStringLiteral("data")).isNull(), "error data is null");
    tests.check(!containsSensitiveText(body), "response does not disclose exception details");

    const QString logName = QStringLiteral("ncs_%1.log").arg(
        QDateTime::currentDateTimeUtc().date().toString(QStringLiteral("yyyyMMdd")));
    QFile logFile(QDir(temporaryDirectory.path()).filePath(logName));
    tests.check(logFile.open(QIODevice::ReadOnly | QIODevice::Text), "exception log can be read");
    const QByteArray logContents = logFile.readAll();
    tests.check(!containsSensitiveText(logContents), "exception log does not disclose exception details");

    const auto lines = logContents.split('\n');
    bool foundExceptionEvent = false;
    for (const QByteArray &line : lines) {
        if (line.trimmed().isEmpty()) {
            continue;
        }
        const QJsonObject event = QJsonDocument::fromJson(line).object();
        if (event.value(QStringLiteral("module")).toString()
                == QStringLiteral("server.exception")) {
            foundExceptionEvent =
                event.value(QStringLiteral("level")).toString() == QStringLiteral("ERROR")
                && event.value(QStringLiteral("requestId")).toString()
                    == QString::fromUtf8(requestId)
                && event.value(QStringLiteral("message")).toString()
                    == QStringLiteral("unhandled request exception");
        }
    }
    tests.check(foundExceptionEvent, "generic exception event keeps request context");
    return tests.result();
}
