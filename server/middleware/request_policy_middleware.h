#pragma once

#include "infrastructure/files/structured_logger.h"

#include <crow.h>

#include <chrono>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace ncs::server::middleware {

class RateLimiter final {
public:
    struct Decision { bool allowed = true; int retryAfterSec = 0; };

    Decision allow(
        std::string_view clientKey,
        std::chrono::steady_clock::time_point now);
    void configure(
        double refillPerSecond,
        double burstCapacity,
        std::size_t maximumBuckets = 4096,
        std::chrono::seconds idleTime = std::chrono::minutes(5));
    void cleanup(std::chrono::steady_clock::time_point now);
    std::size_t size() const;

private:
    struct Bucket {
        double tokens = 0;
        std::chrono::steady_clock::time_point updatedAt{};
        std::list<std::string>::iterator recency;
    };

    void cleanupUnlocked(std::chrono::steady_clock::time_point now);
    void removeOldestUnlocked();

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Bucket> buckets_;
    std::list<std::string> recency_;
    double refillPerSecond_ = 20.0;
    double burstCapacity_ = 50.0;
    std::size_t maximumBuckets_ = 4096;
    std::chrono::seconds idleTime_{300};
};

struct RequestPolicyMiddleware {
    struct context {
        bool initialized = false;
        std::string requestId;
        std::chrono::steady_clock::time_point startedAt;
        std::chrono::seconds deadline{10};
    };

    void configure(
        infrastructure::files::StructuredLogger &logger,
        std::unordered_set<std::string> allowedOrigins = {});
    void before_handle(crow::request &request, crow::response &response, context &context);
    void after_handle(crow::request &request, crow::response &response, context &context);

    static bool validRequestId(std::string_view value);
    static std::size_t bodyLimitForPath(std::string_view path);
    static std::chrono::seconds deadlineForPath(std::string_view path);

    RateLimiter rateLimiter;
    RateLimiter passwordRateLimiter;

private:
    infrastructure::files::StructuredLogger *logger_ = nullptr;
    std::unordered_set<std::string> allowedOrigins_;
};

} // namespace ncs::server::middleware
