#include "infrastructure/ai/llm_config.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace ncs::infrastructure::ai
{
namespace
{

std::string trimmed(const std::string_view value)
{
    const auto first =
        std::find_if_not(value.begin(), value.end(), [](const unsigned char character)
                         { return std::isspace(character) != 0; });
    const auto last =
        std::find_if_not(value.rbegin(), value.rend(),
                         [](const unsigned char character) { return std::isspace(character) != 0; })
            .base();
    if (first >= last)
        return {};
    return std::string(first, last);
}

std::string lowered(const std::string_view value)
{
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](const unsigned char character)
                   { return static_cast<char>(std::tolower(character)); });
    return result;
}

bool isLoopbackHost(const std::string_view remainder)
{
    return remainder.rfind("127.0.0.1", 0) == 0 || remainder.rfind("::1", 0) == 0 ||
           remainder.rfind("[::1]", 0) == 0 || remainder.rfind("localhost", 0) == 0;
}

} // namespace

bool LlmConfig::enabled() const
{
    return !provider.empty() && !model.empty() && !baseUrl.empty() && !apiKey.empty();
}

std::string_view providerDefaultBaseUrl(const std::string_view provider)
{
    const auto name = lowered(provider);
    if (name == "openai")
        return "https://api.openai.com/v1";
    if (name == "deepseek")
        return "https://api.deepseek.com/v1";
    if (name == "qwen" || name == "dashscope")
        return "https://dashscope.aliyuncs.com/compatible-mode/v1";
    if (name == "claude" || name == "anthropic")
        return "https://api.anthropic.com/v1";
    return {};
}

std::vector<std::string_view> supportedLlmProviders()
{
    return {"openai", "deepseek", "qwen", "claude", "custom"};
}

std::optional<std::string> normalizeBaseUrl(const std::string_view baseUrl)
{
    auto value = trimmed(baseUrl);
    while (!value.empty() && value.back() == '/')
        value.pop_back();
    if (value.empty())
        return std::nullopt;

    constexpr std::string_view secure = "https://";
    constexpr std::string_view insecure = "http://";
    if (value.rfind(secure, 0) == 0)
        return value;
    if (value.rfind(insecure, 0) == 0 &&
        isLoopbackHost(std::string_view(value).substr(insecure.size())))
        return value;
    return std::nullopt;
}

std::optional<LlmConfig> makeLlmConfig(const std::string_view provider,
                                       const std::string_view model, const std::string_view baseUrl,
                                       const std::string_view apiKey, const std::int64_t timeoutMs)
{
    const auto key = trimmed(apiKey);
    const auto selectedModel = trimmed(model);
    if (key.empty() || selectedModel.empty())
        return std::nullopt;

    auto selectedProvider = lowered(trimmed(provider));
    if (selectedProvider.empty())
        selectedProvider = "openai";

    std::string endpoint;
    const auto explicitEndpoint = normalizeBaseUrl(baseUrl);
    if (!explicitEndpoint)
    {
        const auto fallback = providerDefaultBaseUrl(selectedProvider);
        if (fallback.empty())
            return std::nullopt;
        endpoint.assign(fallback);
    }
    else
    {
        endpoint = *explicitEndpoint;
    }

    LlmConfig config;
    config.provider = selectedProvider;
    config.model = selectedModel;
    config.baseUrl = std::move(endpoint);
    config.apiKey = key;
    // 上限 60 秒，避免一次大模型调用拖垮有界阻塞工作队列。
    config.timeoutMs = std::clamp<std::int64_t>(timeoutMs, 1000, 60000);
    return config;
}

} // namespace ncs::infrastructure::ai
