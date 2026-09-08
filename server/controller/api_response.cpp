#include "server/controller/api_response.h"

#include "infrastructure/files/structured_logger.h"

#include <QJsonDocument>
#include <QJsonValue>
#include <QString>

#include <array>
#include <cctype>
#include <string>

namespace ncs::server::controller
{
namespace
{

QString fromUtf8(const std::string_view value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

// 判断外部可见文本是否命中脱敏黑名单：小写化后匹配 SQL 关键字、bearer/token/password、
// 堆栈信息与路径穿越等模式，并拦截 /home/、/srv/、盘符等内部路径与连续 11 位以上数字
// （疑似手机号）。命中即视为敏感文本，必须以兜底文案替代。
bool containsSensitivePattern(const std::string_view value)
{
    std::string lower(value);
    for (char& character : lower)
    {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    constexpr std::array<std::string_view, 10> blocked{
        "select ", "insert ", "update ",  "delete ",     "sqlite",
        "bearer ", "token",   "password", "stack trace", "../",
    };
    for (const auto pattern : blocked)
    {
        if (lower.find(pattern) != std::string::npos)
            return true;
    }
    if (value.find("/home/") != std::string_view::npos ||
        value.find("/srv/") != std::string_view::npos ||
        value.find(":\\") != std::string_view::npos)
    {
        return true;
    }
    int consecutiveDigits = 0;
    for (const char character : value)
    {
        consecutiveDigits =
            std::isdigit(static_cast<unsigned char>(character)) ? consecutiveDigits + 1 : 0;
        if (consecutiveDigits >= 11)
            return true;
    }
    return false;
}

// 响应文本的统一脱敏出口：超过 256 字节或命中敏感模式时丢弃原文，改用 fallback 文案。
QString safeText(const std::string_view value, const std::string_view fallback)
{
    if (value.size() > 256 || containsSensitivePattern(value))
        return fromUtf8(fallback);
    return fromUtf8(value);
}

crow::response jsonResponse(const int status, const QJsonObject& object)
{
    crow::response response;
    response.code = status;
    response.body = QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
    response.set_header("Content-Type", "application/json; charset=utf-8");
    applyPublicSecurityHeaders(response);
    return response;
}

QString currentRequestId()
{
    return fromUtf8(infrastructure::files::currentRequestId());
}

} // namespace

// 成功响应信封：code 固定为 0，message/userMessage 先经 safeText 脱敏，并附带当前请求 ID。
crow::response successResponse(QJsonObject data, const int httpStatus,
                               const std::string_view message, const std::string_view userMessage)
{
    return jsonResponse(httpStatus, QJsonObject{
                                        {QStringLiteral("success"), true},
                                        {QStringLiteral("code"), 0},
                                        {QStringLiteral("message"), safeText(message, "")},
                                        {QStringLiteral("userMessage"), safeText(userMessage, "")},
                                        {QStringLiteral("requestId"), currentRequestId()},
                                        {QStringLiteral("data"), std::move(data)},
                                    });
}

// 错误响应信封：按公开错误码映射 HTTP 状态码；message 缺省回退错误码名称，userMessage
// 缺省或被脱敏时回退通用中文提示，data 为空时输出 null。
crow::response errorResponse(const core::domain::ErrorCode code, const std::string_view message,
                             const std::string_view userMessage, QJsonObject data)
{
    const QString safeUserMessage = safeText(userMessage, "请求失败，请稍后重试");
    return jsonResponse(
        core::domain::httpStatus(code),
        QJsonObject{
            {QStringLiteral("success"), false},
            {QStringLiteral("code"), static_cast<int>(code)},
            {QStringLiteral("message"), safeText(message, core::domain::errorCodeName(code))},
            {QStringLiteral("userMessage"),
             safeUserMessage.isEmpty() ? QStringLiteral("请求失败，请稍后重试") : safeUserMessage},
            {QStringLiteral("requestId"), currentRequestId()},
            {QStringLiteral("data"),
             data.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(data)},
        });
}

// 为所有公开响应补齐安全响应头：禁缓存（no-store）、禁 MIME 嗅探、禁 Referer 泄漏与默认 CSP。
void applyPublicSecurityHeaders(crow::response& response)
{
    response.set_header("Cache-Control", "no-store");
    response.set_header("X-Content-Type-Options", "nosniff");
    response.set_header("Referrer-Policy", "no-referrer");
    response.set_header("Content-Security-Policy", "default-src 'none'; frame-ancestors 'none'");
}

} // namespace ncs::server::controller
