#include "infrastructure/ai/llm_client.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <utility>

namespace ncs::infrastructure::ai
{
namespace
{

QJsonObject messageJson(const core::application::LlmMessage& message)
{
    QJsonObject object;
    object.insert(
        QStringLiteral("role"),
        QString::fromLatin1(core::application::llmRoleName(message.role).data(),
                            static_cast<int>(core::application::llmRoleName(message.role).size())));
    if (message.role == core::application::LlmRole::Assistant && !message.toolCalls.empty())
    {
        QJsonArray calls;
        for (const auto& call : message.toolCalls)
        {
            calls.append(QJsonObject{
                {QStringLiteral("id"), QString::fromStdString(call.id)},
                {QStringLiteral("type"), QStringLiteral("function")},
                {QStringLiteral("function"),
                 QJsonObject{
                     {QStringLiteral("name"), QString::fromStdString(call.name)},
                     {QStringLiteral("arguments"), QString::fromStdString(call.argumentsJson)},
                 }},
            });
        }
        object.insert(QStringLiteral("content"),
                      message.content.empty()
                          ? QJsonValue(QJsonValue::Null)
                          : QJsonValue(QString::fromStdString(message.content)));
        object.insert(QStringLiteral("tool_calls"), calls);
        return object;
    }
    if (message.role == core::application::LlmRole::Tool)
    {
        object.insert(QStringLiteral("tool_call_id"), QString::fromStdString(message.toolCallId));
        if (!message.name.empty())
            object.insert(QStringLiteral("name"), QString::fromStdString(message.name));
    }
    object.insert(QStringLiteral("content"), QString::fromStdString(message.content));
    return object;
}

// 模型偶发返回带 Markdown 代码围栏或尾随说明的参数串；这里只做最小清理，
// 仍然把结果交给 QJsonDocument 严格校验，绝不“猜”出不存在的参数。
std::string sanitizeArguments(const QString& raw)
{
    auto text = raw.trimmed();
    if (text.startsWith(QStringLiteral("```")))
    {
        const auto firstBreak = text.indexOf(QLatin1Char('\n'));
        if (firstBreak >= 0)
            text = text.mid(firstBreak + 1);
        if (text.endsWith(QStringLiteral("```")))
            text.chop(3);
    }
    return text.trimmed().toStdString();
}

} // namespace

OpenAiCompatibleLlmClient::OpenAiCompatibleLlmClient(LlmConfig config) : config_(std::move(config))
{
}

bool OpenAiCompatibleLlmClient::available() const
{
    return config_.enabled();
}

QByteArray
OpenAiCompatibleLlmClient::buildRequestBody(const core::application::LlmChatRequest& request,
                                            const std::string& defaultModel)
{
    QJsonArray messages;
    for (const auto& message : request.messages)
        messages.append(messageJson(message));

    QJsonObject body{
        {QStringLiteral("model"),
         QString::fromStdString(request.model.empty() ? defaultModel : request.model)},
        {QStringLiteral("messages"), messages},
    };
    if (!request.tools.empty())
    {
        QJsonArray tools;
        for (const auto& tool : request.tools)
        {
            auto parameters =
                QJsonDocument::fromJson(QByteArray::fromStdString(tool.parametersJson)).object();
            if (parameters.isEmpty())
                parameters = QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}};
            tools.append(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("function")},
                {QStringLiteral("function"),
                 QJsonObject{
                     {QStringLiteral("name"), QString::fromStdString(tool.name)},
                     {QStringLiteral("description"), QString::fromStdString(tool.description)},
                     {QStringLiteral("parameters"), parameters},
                 }},
            });
        }
        body.insert(QStringLiteral("tools"), tools);
        body.insert(QStringLiteral("tool_choice"), QStringLiteral("auto"));
    }
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

std::optional<core::application::LlmChatResponse>
OpenAiCompatibleLlmClient::parseResponse(const QByteArray& payload, std::string* errorDetail)
{
    const auto setError = [errorDetail](const char* detail)
    {
        if (errorDetail)
            *errorDetail = detail;
    };

    QJsonParseError parseError{};
    const auto document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        setError("llm response is not a json object");
        return std::nullopt;
    }
    const auto root = document.object();
    if (root.contains(QStringLiteral("error")))
    {
        // 厂商错误正文可能包含请求片段或端点信息，只留在内部诊断字符串中。
        const auto message = root.value(QStringLiteral("error"))
                                 .toObject()
                                 .value(QStringLiteral("message"))
                                 .toString();
        if (errorDetail)
            *errorDetail =
                message.isEmpty() ? "llm provider reported an error" : message.toStdString();
        return std::nullopt;
    }
    const auto choices = root.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty())
    {
        setError("llm response has no choices");
        return std::nullopt;
    }
    const auto first = choices.first().toObject();
    const auto message = first.value(QStringLiteral("message")).toObject();

    core::application::LlmChatResponse response;
    response.content = message.value(QStringLiteral("content")).toString().toStdString();
    response.finishReason = first.value(QStringLiteral("finish_reason")).toString().toStdString();
    response.model = root.value(QStringLiteral("model")).toString().toStdString();
    const auto usage = root.value(QStringLiteral("usage")).toObject();
    response.usage.promptTokens = usage.value(QStringLiteral("prompt_tokens")).toInteger(0);
    response.usage.completionTokens = usage.value(QStringLiteral("completion_tokens")).toInteger(0);

    for (const auto& entry : message.value(QStringLiteral("tool_calls")).toArray())
    {
        const auto call = entry.toObject();
        const auto function = call.value(QStringLiteral("function")).toObject();
        core::application::LlmToolCall parsed;
        parsed.id = call.value(QStringLiteral("id")).toString().toStdString();
        parsed.name = function.value(QStringLiteral("name")).toString().toStdString();
        parsed.argumentsJson =
            sanitizeArguments(function.value(QStringLiteral("arguments")).toString());
        if (parsed.name.empty())
        {
            setError("llm tool call is missing a function name");
            return std::nullopt;
        }
        if (parsed.id.empty())
            parsed.id = "call_" + std::to_string(response.toolCalls.size() + 1);
        response.toolCalls.push_back(std::move(parsed));
    }

    if (response.content.empty() && response.toolCalls.empty())
    {
        setError("llm response carries neither content nor tool calls");
        return std::nullopt;
    }
    return response;
}

core::application::ServiceResult<core::application::LlmChatResponse>
OpenAiCompatibleLlmClient::chat(const core::application::LlmChatRequest& request)
{
    using core::application::LlmChatResponse;
    using core::domain::ErrorCode;

    if (!available())
        return {ErrorCode::ExternalServiceUnavailable, std::nullopt};

    const QUrl endpoint(QString::fromStdString(config_.baseUrl + "/chat/completions"));
    if (!endpoint.isValid())
        return {ErrorCode::ExternalServiceUnavailable, std::nullopt};

    QNetworkRequest networkRequest(endpoint);
    networkRequest.setHeader(QNetworkRequest::ContentTypeHeader,
                             QStringLiteral("application/json; charset=utf-8"));
    networkRequest.setRawHeader("Accept", "application/json");
    // Key 只出现在请求头中；异常路径下也不会被写进日志或响应。
    networkRequest.setRawHeader("Authorization",
                                QByteArray("Bearer ") + QByteArray::fromStdString(config_.apiKey));

    const auto timeoutMs = static_cast<int>(
        request.timeoutMs > 0 ? std::min<std::int64_t>(request.timeoutMs, config_.timeoutMs)
                              : config_.timeoutMs);
    networkRequest.setTransferTimeout(timeoutMs);

    QNetworkAccessManager manager;
    QEventLoop loop;
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    const auto body = buildRequestBody(request, config_.model);
    QNetworkReply* reply = manager.post(networkRequest, body);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    if (!reply->isFinished())
        reply->abort();

    const auto httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto payload = reply->readAll();
    const auto networkError = reply->error();
    reply->deleteLater();

    if (networkError != QNetworkReply::NoError || httpStatus < 200 || httpStatus >= 300)
        return {ErrorCode::ExternalServiceUnavailable, std::nullopt};

    std::string detail;
    auto parsed = parseResponse(payload, &detail);
    if (!parsed)
        return {ErrorCode::InternalError, std::nullopt};
    return {ErrorCode::Ok, std::move(parsed)};
}

} // namespace ncs::infrastructure::ai
