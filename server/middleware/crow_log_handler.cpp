#include "server/middleware/crow_log_handler.h"

#include <QRegularExpression>
#include <QString>

#include <cstdio>
#include <exception>
#include <string>

namespace ncs::server::middleware {
namespace {

using ncs::infrastructure::files::LogLevel;

LogLevel fromCrowLogLevel(const crow::LogLevel level)
{
    switch (level) {
    case crow::LogLevel::Debug:
        return LogLevel::Debug;
    case crow::LogLevel::Info:
        return LogLevel::Info;
    case crow::LogLevel::Warning:
        return LogLevel::Warning;
    case crow::LogLevel::Error:
        return LogLevel::Error;
    case crow::LogLevel::Critical:
        return LogLevel::Critical;
    }
    return LogLevel::Error;
}

std::string sanitizeCrowMessage(const std::string &message)
{
    QString sanitized = QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size()));
    static const QRegularExpression queryPattern(QStringLiteral("\\?[^\\s]*"));
    static const QRegularExpression bearerPattern(
        QStringLiteral("Bearer\\s+[^\\s]+"),
        QRegularExpression::CaseInsensitiveOption);
    sanitized.replace(queryPattern, QStringLiteral("?<redacted>"));
    sanitized.replace(bearerPattern, QStringLiteral("Bearer <redacted>"));
    const auto bytes = sanitized.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

} // namespace

CrowLogHandler::CrowLogHandler(ncs::infrastructure::files::StructuredLogger &logger)
    : logger_(logger)
{
}

void CrowLogHandler::log(const std::string &message, const crow::LogLevel level)
{
    try {
        logger_.log(fromCrowLogLevel(level), "crow", sanitizeCrowMessage(message));
    } catch (const std::exception &) {
        std::fputs(
            "{\"level\":\"ERROR\",\"module\":\"logging\","
            "\"message\":\"Crow log write failed\"}\n",
            stderr);
        std::fflush(stderr);
    }
}

crow::LogLevel toCrowLogLevel(const ncs::infrastructure::files::LogLevel level)
{
    switch (level) {
    case LogLevel::Debug:
        return crow::LogLevel::Debug;
    case LogLevel::Info:
        return crow::LogLevel::Info;
    case LogLevel::Warning:
        return crow::LogLevel::Warning;
    case LogLevel::Error:
        return crow::LogLevel::Error;
    case LogLevel::Critical:
        return crow::LogLevel::Critical;
    }
    return crow::LogLevel::Error;
}

} // namespace ncs::server::middleware
