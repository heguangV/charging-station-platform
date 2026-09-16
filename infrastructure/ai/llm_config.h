// LLM 接入配置：把进程环境变量（AI_PROVIDER/AI_MODEL/AI_BASE_URL/AI_API_KEY）解析为
// 厂商无关的 LlmConfig，并把 provider 名映射到 OpenAI-compatible 根地址。
// 约束：真实 Key 只来自进程环境或本地 .env，绝不写入代码、日志或响应；端点默认必须是
// HTTPS，仅允许本机开发指向回环 HTTP。配置不完整一律视为“未启用 AI”，由 Agent 走确定性降级。

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ncs::infrastructure::ai
{

struct LlmConfig
{
    std::string provider = "openai";
    std::string model;
    std::string baseUrl;
    std::string apiKey;
    std::int64_t timeoutMs = 15000;

    // provider/model/baseUrl/apiKey 齐全才视为可用。
    bool enabled() const;
};

// 已知 provider 的默认 OpenAI-compatible 根地址；未知 provider 返回空串，
// 调用方必须显式提供 AI_BASE_URL。
std::string_view providerDefaultBaseUrl(std::string_view provider);

std::vector<std::string_view> supportedLlmProviders();

// 归一化根地址：去首尾空白、去尾部 '/'；必须是 https://，或仅本机回环的 http://。
// 其它形式（含非回环明文 HTTP、非 http(s) 协议）返回 nullopt。
std::optional<std::string> normalizeBaseUrl(std::string_view baseUrl);

// 组装配置。model 或 apiKey 为空、端点非法、或 provider 未知且未给出端点时返回 nullopt。
std::optional<LlmConfig> makeLlmConfig(std::string_view provider, std::string_view model,
                                       std::string_view baseUrl, std::string_view apiKey,
                                       std::int64_t timeoutMs);

} // namespace ncs::infrastructure::ai
