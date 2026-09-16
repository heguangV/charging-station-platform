// OpenAI-compatible 大模型客户端：实现 core::application::LlmClient，负责 HTTP 请求、
// Bearer 鉴权、超时、错误映射与 JSON 解析，使 Agent 核心逻辑不依赖任何厂商特有协议。
// 约束：沿用仓库既有外部调用模式（QEventLoop + QTimer 超时把异步 Qt Network 包成同步调用），
// 只在有界阻塞工作队列中被调用；失败一律映射为 ErrorCode，不抛异常、不把厂商错误正文
// 回传给客户端，也不把 API Key 写进日志。

#pragma once

#include "core/application/llm_client.h"
#include "infrastructure/ai/llm_config.h"

#include <QByteArray>

#include <optional>
#include <string>

namespace ncs::infrastructure::ai
{

class OpenAiCompatibleLlmClient final : public core::application::LlmClient
{
  public:
    explicit OpenAiCompatibleLlmClient(LlmConfig config);

    bool available() const override;

    core::application::ServiceResult<core::application::LlmChatResponse>
    chat(const core::application::LlmChatRequest& request) override;

    // 纯函数：供离线单元测试断言请求体与响应解析，不发起网络请求。
    static QByteArray buildRequestBody(const core::application::LlmChatRequest& request,
                                       const std::string& defaultModel);
    static std::optional<core::application::LlmChatResponse>
    parseResponse(const QByteArray& payload, std::string* errorDetail);

    const LlmConfig& config() const
    {
        return config_;
    }

  private:
    LlmConfig config_;
};

} // namespace ncs::infrastructure::ai
