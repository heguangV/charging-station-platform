#include "server/middleware/request_policy_middleware.h"

#include "core/application/idempotency_service.h"
#include "core/domain/error_code.h"
#include "server/controller/api_response.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

#include <asio/ip/address.hpp>

#include <algorithm>
#include <cmath>

namespace ncs::server::middleware
{
namespace
{

bool containsUrlToken(const crow::request& request)
{
    const std::string& url = request.raw_url.empty() ? request.url : request.raw_url;
    const auto query = url.find('?');
    if (query == std::string::npos)
        return false;
    std::string lower = url.substr(query + 1);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](const unsigned char value)
                   { return static_cast<char>(std::tolower(value)); });
    return lower.find("token=") != std::string::npos ||
           lower.find("accesstoken=") != std::string::npos ||
           lower.find("authorization=") != std::string::npos;
}

std::string generatedRequestId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
}

std::string logRoute(const std::string_view path)
{
    constexpr std::string_view prefix = "/api/v1/";
    if (path.rfind(prefix, 0) != 0)
        return "non-api";
    const auto nextSlash = path.find('/', prefix.size());
    return std::string(path.substr(prefix.size(), nextSlash == std::string_view::npos
                                                      ? path.size() - prefix.size()
                                                      : nextSlash - prefix.size()));
}

// 限流客户端键：IPv4 直接使用地址；IPv6 聚合到 /64 前缀，防止同一子网用大量地址绕过限流；
// 解析失败统一记为 unknown。
std::string clientRateLimitKey(const std::string_view addressText)
{
    asio::error_code error;
    auto address = asio::ip::make_address(addressText, error);
    if (error)
        return "unknown";
    if (!address.is_v6())
        return address.to_string();
    auto bytes = address.to_v6().to_bytes();
    std::fill(bytes.begin() + 8, bytes.end(), 0);
    return asio::ip::address_v6(bytes).to_string() + "/64";
}

bool passwordWorkPath(const std::string_view path)
{
    return path == "/api/v1/user/auth/login/password";
}

void setEnvelopeRequestId(crow::response& response, const std::string_view requestId)
{
    if (response.get_header_value("Content-Type").find("application/json") == std::string::npos)
    {
        return;
    }
    QJsonParseError error;
    QJsonDocument document =
        QJsonDocument::fromJson(QByteArray::fromStdString(response.body), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return;
    QJsonObject object = document.object();
    if (!object.contains(QStringLiteral("requestId")))
        return;
    object.insert(QStringLiteral("requestId"), QString::fromStdString(std::string(requestId)));
    response.body = QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
}

} // namespace

// 令牌桶限流判定：按流逝时间补充令牌（上限 burstCapacity），有令牌则放行并扣减 1 个，
// 拒绝时返回按速率换算的等待秒数；桶数达到上限时淘汰最久未使用的桶，补充与淘汰均在
// 同一把互斥锁内完成。
RateLimiter::Decision RateLimiter::allow(const std::string_view clientKey,
                                         const std::chrono::steady_clock::time_point now)
{
    std::lock_guard lock(mutex_);
    cleanupUnlocked(now);
    const std::string key(clientKey);
    auto found = buckets_.find(key);
    if (found == buckets_.end())
    {
        while (buckets_.size() >= maximumBuckets_)
            removeOldestUnlocked();
        recency_.push_back(key);
        found = buckets_
                    .emplace(key,
                             Bucket{
                                 burstCapacity_,
                                 now,
                                 std::prev(recency_.end()),
                             })
                    .first;
    }
    else
    {
        recency_.splice(recency_.end(), recency_, found->second.recency);
        found->second.recency = std::prev(recency_.end());
    }
    Bucket& bucket = found->second;
    const double elapsed = std::chrono::duration<double>(now - bucket.updatedAt).count();
    bucket.tokens = std::min(burstCapacity_, bucket.tokens + elapsed * refillPerSecond_);
    bucket.updatedAt = now;
    if (bucket.tokens >= 1.0)
    {
        bucket.tokens -= 1.0;
        return {};
    }
    return {false, std::max(1, static_cast<int>(std::ceil(1.0 / refillPerSecond_)))};
}

void RateLimiter::configure(const double refillPerSecond, const double burstCapacity,
                            const std::size_t maximumBuckets, const std::chrono::seconds idleTime)
{
    if (refillPerSecond <= 0 || burstCapacity < 1 || maximumBuckets == 0 ||
        idleTime <= std::chrono::seconds::zero())
        return;
    std::lock_guard lock(mutex_);
    refillPerSecond_ = refillPerSecond;
    burstCapacity_ = burstCapacity;
    maximumBuckets_ = maximumBuckets;
    idleTime_ = idleTime;
    buckets_.clear();
    recency_.clear();
}

void RateLimiter::cleanup(const std::chrono::steady_clock::time_point now)
{
    std::lock_guard lock(mutex_);
    cleanupUnlocked(now);
}

std::size_t RateLimiter::size() const
{
    std::lock_guard lock(mutex_);
    return buckets_.size();
}

void RateLimiter::cleanupUnlocked(const std::chrono::steady_clock::time_point now)
{
    while (!recency_.empty())
    {
        const auto found = buckets_.find(recency_.front());
        if (found == buckets_.end())
        {
            recency_.pop_front();
            continue;
        }
        if (now - found->second.updatedAt <= idleTime_)
            break;
        buckets_.erase(found);
        recency_.pop_front();
    }
}

void RateLimiter::removeOldestUnlocked()
{
    if (recency_.empty())
        return;
    buckets_.erase(recency_.front());
    recency_.pop_front();
}

void RequestPolicyMiddleware::configure(infrastructure::files::StructuredLogger& logger,
                                        std::unordered_set<std::string> allowedOrigins,
                                        const bool transportSecurityEnabled)
{
    logger_ = &logger;
    allowedOrigins_ = std::move(allowedOrigins);
    transportSecurityEnabled_ = transportSecurityEnabled;
    passwordRateLimiter.configure(0.2, 5.0, 4096, std::chrono::minutes(15));
}

// before 钩子：校验/补发 X-Request-ID，依次执行 Origin 白名单、URL 凭证拦截、请求体上限
// 与按客户端的令牌桶限流，密码登录路径另有更严格的独立限流；任一检查不过即以
// 403/400/413/429 短路响应，不进入路由处理。
void RequestPolicyMiddleware::before_handle(crow::request& request, crow::response& response,
                                            context& context)
{
    context.initialized = true;
    context.startedAt = std::chrono::steady_clock::now();
    context.deadline = deadlineForPath(request.url);
    const std::string& incomingId = request.get_header_value("X-Request-ID");
    context.requestId = validRequestId(incomingId) ? incomingId : generatedRequestId();

    const std::string& origin = request.get_header_value("Origin");
    if (!origin.empty() && allowedOrigins_.find(origin) == allowedOrigins_.end())
    {
        response = controller::errorResponse(core::domain::ErrorCode::Forbidden,
                                             "origin is not allowed", "不允许跨域访问");
        response.end();
        return;
    }
    if (request.method == crow::HTTPMethod::Options && !origin.empty())
    {
        response.code = 204;
        response.end();
        return;
    }
    if (containsUrlToken(request))
    {
        response = controller::errorResponse(core::domain::ErrorCode::InvalidArgument,
                                             "credentials are not allowed in URL",
                                             "认证信息不能放在网址中");
        response.end();
        return;
    }
    if (request.body.size() > bodyLimitForPath(request.url))
    {
        response = controller::errorResponse(core::domain::ErrorCode::InvalidArgument,
                                             "request body too large", "请求内容超过大小限制");
        response.code = 413;
        response.end();
        return;
    }
    const std::string clientKey = clientRateLimitKey(request.remote_ip_address);
    const auto rateDecision = rateLimiter.allow(clientKey, context.startedAt);
    if (!rateDecision.allowed)
    {
        response = controller::errorResponse(
            core::domain::ErrorCode::RateLimited, "request rate exceeded",
            "请求过于频繁，请稍后重试",
            QJsonObject{{QStringLiteral("retryAfterSec"), rateDecision.retryAfterSec}});
        response.set_header("Retry-After", std::to_string(rateDecision.retryAfterSec));
        response.end();
        return;
    }
    if (passwordWorkPath(request.url))
    {
        const auto passwordDecision = passwordRateLimiter.allow(clientKey, context.startedAt);
        if (!passwordDecision.allowed)
        {
            response = controller::errorResponse(
                core::domain::ErrorCode::RateLimited, "password authentication rate exceeded",
                "登录尝试过于频繁，请稍后重试",
                QJsonObject{{QStringLiteral("retryAfterSec"), passwordDecision.retryAfterSec}});
            response.set_header("Retry-After", std::to_string(passwordDecision.retryAfterSec));
            response.end();
        }
    }
}

// after 钩子：兜底初始化缺失的上下文并复查 Origin，超过截止时间的请求改写为超时错误响应；
// 统一回写 X-Request-ID、HSTS（启用 TLS 时）与公开安全头，补 CORS 头，把 JSON 信封内的
// requestId 同步为本次请求 ID，最后输出带路由类别/状态码/耗时的访问日志。
void RequestPolicyMiddleware::after_handle(crow::request& request, crow::response& response,
                                           context& context)
{
    const auto now = std::chrono::steady_clock::now();
    const bool initialized = context.initialized;
    if (!initialized)
    {
        context.initialized = true;
        context.startedAt = now;
        context.deadline = deadlineForPath(request.url);
        const std::string& incomingId = request.get_header_value("X-Request-ID");
        context.requestId = validRequestId(incomingId) ? incomingId : generatedRequestId();
    }
    const auto elapsed = now - context.startedAt;
    const std::string& origin = request.get_header_value("Origin");
    const bool originAllowed =
        origin.empty() || allowedOrigins_.find(origin) != allowedOrigins_.end();
    if (!originAllowed && !initialized)
    {
        response = controller::errorResponse(core::domain::ErrorCode::Forbidden,
                                             "origin is not allowed", "不允许跨域访问");
    }
    else if (response.code == 413 && response.body.empty())
    {
        response = controller::errorResponse(core::domain::ErrorCode::InvalidArgument,
                                             "request body too large", "请求内容超过大小限制");
        response.code = 413;
    }
    else if (initialized && elapsed > context.deadline)
    {
        response = controller::errorResponse(core::domain::ErrorCode::ExternalServiceUnavailable,
                                             "request deadline exceeded", "请求处理超时，请重试");
    }
    response.set_header("X-Request-ID", context.requestId);
    if (transportSecurityEnabled_)
    {
        response.set_header("Strict-Transport-Security", "max-age=31536000");
    }
    controller::applyPublicSecurityHeaders(response);
    if (!origin.empty() && originAllowed)
    {
        response.set_header("Access-Control-Allow-Origin", origin);
        response.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        response.set_header("Access-Control-Allow-Headers",
                            "Authorization, Content-Type, X-Request-ID, Idempotency-Key");
        response.set_header("Access-Control-Max-Age", "600");
        response.set_header("Vary", "Origin");
    }
    setEnvelopeRequestId(response, context.requestId);
    if (logger_)
    {
        try
        {
            const auto milliseconds =
                std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
            logger_->log(infrastructure::files::LogLevel::Info, "server.http",
                         "request category=" + logRoute(request.url) +
                             " status=" + std::to_string(response.code) +
                             " durationMs=" + std::to_string(milliseconds.count()),
                         context.requestId);
        }
        catch (...)
        {
        }
    }
}

bool RequestPolicyMiddleware::validRequestId(const std::string_view value)
{
    return core::application::IdempotencyService::isUuid(value);
}

std::size_t RequestPolicyMiddleware::bodyLimitForPath(const std::string_view path)
{
    if (path == "/api/v1/internal/ml/predictions/batch")
        return 8 * 1024 * 1024;
    if (path == "/api/v1/user/me/avatar")
        return 5 * 1024 * 1024;
    return 1024 * 1024;
}

// 按路径返回请求处理截止时间：统计类 /stats/ 路径放宽到 30 秒，其余统一 10 秒。
std::chrono::seconds RequestPolicyMiddleware::deadlineForPath(const std::string_view path)
{
    return path.find("/stats/") != std::string_view::npos ? std::chrono::seconds(30)
                                                          : std::chrono::seconds(10);
}

} // namespace ncs::server::middleware
