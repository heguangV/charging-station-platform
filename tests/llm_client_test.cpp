// LLM 基础设施测试：配置解析与端点白名单、请求体序列化、响应解析与失败路径。
// 全部为纯函数断言，不发起网络请求；用来固定“厂商可替换”的 OpenAI-compatible 协议边界。
#include "infrastructure/ai/llm_client.h"
#include "infrastructure/ai/llm_config.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <iostream>
#include <string>
#include <string_view>

namespace
{

class TestRunner final
{
  public:
    void check(const bool condition, const std::string_view message)
    {
        if (!condition)
        {
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

using ncs::core::application::LlmChatRequest;
using ncs::core::application::LlmMessage;
using ncs::core::application::LlmRole;
using ncs::core::application::LlmToolCall;
using ncs::core::application::LlmToolSpec;
using ncs::infrastructure::ai::LlmConfig;
using ncs::infrastructure::ai::makeLlmConfig;
using ncs::infrastructure::ai::normalizeBaseUrl;
using ncs::infrastructure::ai::OpenAiCompatibleLlmClient;
using ncs::infrastructure::ai::providerDefaultBaseUrl;

QJsonObject parse(const QByteArray& payload)
{
    return QJsonDocument::fromJson(payload).object();
}

} // namespace

int main()
{
    TestRunner tests;

    // ================= 配置 =================
    {
        tests.check(providerDefaultBaseUrl("openai") == "https://api.openai.com/v1",
                    "openai must map to its official OpenAI-compatible endpoint");
        tests.check(providerDefaultBaseUrl("DeepSeek") == "https://api.deepseek.com/v1",
                    "provider names must be matched case-insensitively");
        tests.check(!providerDefaultBaseUrl("qwen").empty() &&
                        !providerDefaultBaseUrl("claude").empty(),
                    "qwen and claude must have documented compatible endpoints");
        tests.check(providerDefaultBaseUrl("unknown-vendor").empty(),
                    "an unknown provider must not silently get an endpoint");

        tests.check(normalizeBaseUrl("https://api.example.com/v1/") == "https://api.example.com/v1",
                    "trailing slashes must be normalised away");
        tests.check(normalizeBaseUrl("  https://api.example.com/v1  ") ==
                        "https://api.example.com/v1",
                    "surrounding whitespace must be trimmed");
        tests.check(!normalizeBaseUrl("http://api.example.com/v1").has_value(),
                    "plain HTTP to a remote host must be rejected");
        tests.check(!normalizeBaseUrl("ftp://api.example.com").has_value(),
                    "non-HTTP schemes must be rejected");
        tests.check(!normalizeBaseUrl("").has_value(), "an empty endpoint must be rejected");
        tests.check(normalizeBaseUrl("http://127.0.0.1:8080/v1").has_value(),
                    "loopback HTTP is allowed for local development only");
        tests.check(normalizeBaseUrl("http://localhost:8080/v1").has_value(),
                    "localhost HTTP is allowed for local development only");

        tests.check(!makeLlmConfig("openai", "", "", "key", 15000).has_value(),
                    "a missing model must disable the AI client");
        tests.check(!makeLlmConfig("openai", "gpt", "", "", 15000).has_value(),
                    "a missing API key must disable the AI client");
        tests.check(!makeLlmConfig("unknown-vendor", "model", "", "key", 15000).has_value(),
                    "an unknown provider without an explicit endpoint must be rejected");

        const auto openai = makeLlmConfig("openai", "gpt-4o-mini", "", "secret-key", 5000);
        tests.check(openai.has_value() && openai->baseUrl == "https://api.openai.com/v1" &&
                        openai->model == "gpt-4o-mini" && openai->enabled(),
                    "a complete openai configuration must resolve to its default endpoint");
        const auto custom = makeLlmConfig("deepseek", "deepseek-chat",
                                          "https://proxy.example.com/v1", "secret-key", 5000);
        tests.check(custom.has_value() && custom->baseUrl == "https://proxy.example.com/v1",
                    "an explicit endpoint must override the provider default");
        const auto clamped = makeLlmConfig("openai", "gpt", "", "key", 9999999);
        tests.check(clamped.has_value() && clamped->timeoutMs == 60000,
                    "an absurd timeout must be clamped to the upper bound");
        const auto tooSmall = makeLlmConfig("openai", "gpt", "", "key", 1);
        tests.check(tooSmall.has_value() && tooSmall->timeoutMs == 1000,
                    "a tiny timeout must be clamped to the lower bound");
    }

    // ================= 未配置时不得发起请求 =================
    {
        OpenAiCompatibleLlmClient disabled{LlmConfig{}};
        tests.check(!disabled.available(), "an empty configuration must report unavailable");
        const auto result = disabled.chat(LlmChatRequest{});
        tests.check(!result.ok() &&
                        result.error == ncs::core::domain::ErrorCode::ExternalServiceUnavailable,
                    "chat must fail with ExternalServiceUnavailable when unconfigured");
    }

    // ================= 请求体序列化 =================
    {
        LlmChatRequest request;
        LlmMessage system;
        system.role = LlmRole::System;
        system.content = "系统提示";
        LlmMessage user;
        user.role = LlmRole::User;
        user.content = "帮我找充电站";
        request.messages = {system, user};
        LlmToolSpec spec;
        spec.name = "station_search";
        spec.description = "检索附近充电站";
        spec.parametersJson = R"({"type":"object","properties":{"limit":{"type":"integer"}}})";
        request.tools = {spec};

        const auto body =
            parse(OpenAiCompatibleLlmClient::buildRequestBody(request, "gpt-4o-mini"));
        tests.check(body.value(QStringLiteral("model")).toString() == QStringLiteral("gpt-4o-mini"),
                    "the default model must be used when the request does not override it");
        const auto messages = body.value(QStringLiteral("messages")).toArray();
        tests.check(messages.size() == 2 &&
                        messages[0].toObject().value(QStringLiteral("role")).toString() ==
                            QStringLiteral("system") &&
                        messages[1].toObject().value(QStringLiteral("role")).toString() ==
                            QStringLiteral("user"),
                    "message roles must be serialised in order");
        tests.check(messages[1].toObject().value(QStringLiteral("content")).toString() ==
                        QString::fromUtf8("帮我找充电站"),
                    "user content must be transported as UTF-8 text");
        const auto tools = body.value(QStringLiteral("tools")).toArray();
        tests.check(tools.size() == 1 &&
                        tools.first().toObject().value(QStringLiteral("type")).toString() ==
                            QStringLiteral("function") &&
                        tools.first()
                                .toObject()
                                .value(QStringLiteral("function"))
                                .toObject()
                                .value(QStringLiteral("name"))
                                .toString() == QStringLiteral("station_search"),
                    "tool specs must use the OpenAI function shape");
        tests.check(tools.first()
                        .toObject()
                        .value(QStringLiteral("function"))
                        .toObject()
                        .value(QStringLiteral("parameters"))
                        .isObject(),
                    "tool parameters must be embedded as a JSON schema object");
        tests.check(body.value(QStringLiteral("tool_choice")).toString() == QStringLiteral("auto"),
                    "tool choice must default to auto when tools are advertised");
        tests.check(
            std::string(
                OpenAiCompatibleLlmClient::buildRequestBody(LlmChatRequest{}, "model").constData())
                    .find("\"tools\"") == std::string::npos,
            "tools must be omitted from the body when none are advertised");

        // 模型显式覆盖模型名
        LlmChatRequest overridden = request;
        overridden.model = "deepseek-chat";
        tests.check(parse(OpenAiCompatibleLlmClient::buildRequestBody(overridden, "gpt-4o-mini"))
                            .value(QStringLiteral("model"))
                            .toString() == QStringLiteral("deepseek-chat"),
                    "a per-request model override must win over the default");

        // assistant 工具调用消息必须保留 tool_calls 结构
        LlmChatRequest withCall;
        LlmMessage assistant;
        assistant.role = LlmRole::Assistant;
        LlmToolCall call;
        call.id = "call_1";
        call.name = "station_search";
        call.argumentsJson = R"({"limit":3})";
        assistant.toolCalls = {call};
        LlmMessage observation;
        observation.role = LlmRole::Tool;
        observation.toolCallId = "call_1";
        observation.content = "找到 3 个站点";
        withCall.messages = {assistant, observation};
        const auto callBody = parse(OpenAiCompatibleLlmClient::buildRequestBody(withCall, "m"));
        const auto callMessages = callBody.value(QStringLiteral("messages")).toArray();
        tests.check(
            callMessages.size() == 2 &&
                callMessages[0].toObject().contains(QStringLiteral("tool_calls")) &&
                callMessages[1].toObject().value(QStringLiteral("role")).toString() ==
                    QStringLiteral("tool") &&
                callMessages[1].toObject().value(QStringLiteral("tool_call_id")).toString() ==
                    QStringLiteral("call_1"),
            "assistant tool calls and tool results must round-trip with matching ids");
    }

    // ================= 响应解析 =================
    {
        std::string detail;
        const auto plain = OpenAiCompatibleLlmClient::parseResponse(
            R"({"model":"gpt-4o-mini","choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"你好"}}],"usage":{"prompt_tokens":12,"completion_tokens":3}})",
            &detail);
        tests.check(plain.has_value() && plain->content == "你好" && plain->toolCalls.empty() &&
                        plain->finishReason == "stop" && plain->model == "gpt-4o-mini" &&
                        plain->usage.promptTokens == 12 && plain->usage.completionTokens == 3,
                    "a plain completion must be parsed with content, model and usage");

        const auto withTools = OpenAiCompatibleLlmClient::parseResponse(
            R"({"choices":[{"finish_reason":"tool_calls","message":{"content":null,"tool_calls":[{"id":"call_9","type":"function","function":{"name":"poi_search","arguments":"{\"category\":\"咖啡\"}"}}]}}]})",
            &detail);
        tests.check(withTools.has_value() && withTools->toolCalls.size() == 1 &&
                        withTools->toolCalls.front().id == "call_9" &&
                        withTools->toolCalls.front().name == "poi_search" &&
                        withTools->toolCalls.front().argumentsJson == R"({"category":"咖啡"})",
                    "tool calls must be parsed with id, name and raw arguments");

        // 代码围栏包裹的参数必须被清理，但仍要能解析
        const auto fenced = OpenAiCompatibleLlmClient::parseResponse(
            R"({"choices":[{"message":{"tool_calls":[{"id":"c1","function":{"name":"route","arguments":"```json\n{\"mode\":\"driving\"}\n```"}}]}}]})",
            &detail);
        tests.check(fenced.has_value() && fenced->toolCalls.size() == 1 &&
                        fenced->toolCalls.front().argumentsJson == R"({"mode":"driving"})",
                    "markdown-fenced arguments must be unwrapped");

        // 缺失 tool call id 时补一个稳定的本地 id
        const auto anonymousCall = OpenAiCompatibleLlmClient::parseResponse(
            R"({"choices":[{"message":{"tool_calls":[{"function":{"name":"route","arguments":"{}"}}]}}]})",
            &detail);
        tests.check(anonymousCall.has_value() && anonymousCall->toolCalls.size() == 1 &&
                        !anonymousCall->toolCalls.front().id.empty(),
                    "a tool call without an id must receive a local id");

        // 失败路径
        detail.clear();
        tests.check(!OpenAiCompatibleLlmClient::parseResponse("not json", &detail).has_value() &&
                        !detail.empty(),
                    "a non-JSON body must fail with a diagnostic");
        detail.clear();
        tests.check(!OpenAiCompatibleLlmClient::parseResponse(
                         R"({"error":{"message":"quota exceeded"}})", &detail)
                            .has_value() &&
                        detail == "quota exceeded",
                    "a provider error envelope must be detected and kept for internal diagnosis");
        detail.clear();
        tests.check(
            !OpenAiCompatibleLlmClient::parseResponse(R"({"choices":[]})", &detail).has_value(),
            "an empty choices array must fail");
        detail.clear();
        tests.check(!OpenAiCompatibleLlmClient::parseResponse(
                         R"({"choices":[{"message":{"content":""}}]})", &detail)
                         .has_value(),
                    "an empty completion must fail instead of reporting success");
        detail.clear();
        tests.check(
            !OpenAiCompatibleLlmClient::parseResponse(
                 R"({"choices":[{"message":{"tool_calls":[{"id":"c1","function":{"arguments":"{}"}}]}}]})",
                 &detail)
                 .has_value(),
            "a tool call without a function name must fail");
        detail.clear();
        tests.check(!OpenAiCompatibleLlmClient::parseResponse("[]", &detail).has_value(),
                    "a JSON array body must fail");
    }

    if (tests.result() == 0)
        std::cout << "llm client tests passed\n";
    return tests.result();
}
