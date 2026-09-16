// 大模型对话端口（端口-适配器模式）：定义消息、工具声明、工具调用与对话响应的值类型。
// 具体厂商协议（OpenAI / DeepSeek / Qwen / 其它 OpenAI-compatible 服务）由
// infrastructure/ai 适配，core 与 agent 只依赖本抽象，不感知任何厂商特有字段。
// 约束：API Key 只存在于服务端进程环境，不得进入本端口的响应或日志；实现必须自行处理
// 超时与 JSON 校验并把失败映射为 ErrorCode，不得把异常抛给调用方。

#pragma once

#include "core/application/service_result.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ncs::core::application
{

enum class LlmRole
{
    System,
    User,
    Assistant,
    Tool,
};

constexpr std::string_view llmRoleName(const LlmRole role)
{
    switch (role)
    {
    case LlmRole::System:
        return "system";
    case LlmRole::User:
        return "user";
    case LlmRole::Assistant:
        return "assistant";
    case LlmRole::Tool:
        return "tool";
    }
    return "user";
}

struct LlmToolCall
{
    std::string id;
    std::string name;
    // 模型给出的原始参数 JSON 对象字符串，由调用方解析并校验，不得直接当作可信输入。
    std::string argumentsJson;
};

struct LlmMessage
{
    LlmRole role = LlmRole::User;
    std::string content;
    // Tool 角色消息必须带回它所响应的 toolCallId；Assistant 消息可携带 toolCalls。
    std::string toolCallId;
    std::string name;
    std::vector<LlmToolCall> toolCalls;
};

struct LlmToolSpec
{
    std::string name;
    std::string description;
    // 参数 JSON Schema 对象字符串。
    std::string parametersJson;
};

struct LlmChatRequest
{
    std::vector<LlmMessage> messages;
    std::vector<LlmToolSpec> tools;
    // 留空表示使用配置中的默认模型。
    std::string model;
    // 0 表示使用配置中的默认超时。
    std::int64_t timeoutMs = 0;
};

struct LlmUsage
{
    std::int64_t promptTokens = 0;
    std::int64_t completionTokens = 0;
};

struct LlmChatResponse
{
    std::string content;
    std::vector<LlmToolCall> toolCalls;
    std::string finishReason;
    std::string model;
    LlmUsage usage;
};

class LlmClient
{
  public:
    virtual ~LlmClient() = default;

    // 未配置 Provider/Key/Model 时返回 false；调用方必须据此走确定性降级路径，
    // 而不是把“未配置”当成请求失败。
    virtual bool available() const = 0;

    // 一次对话补全。失败返回带 ErrorCode 的 ServiceResult：
    // 未配置或外部不可用为 ExternalServiceUnavailable，响应无法解析为 InternalError。
    virtual ServiceResult<LlmChatResponse> chat(const LlmChatRequest& request) = 0;
};

} // namespace ncs::core::application
