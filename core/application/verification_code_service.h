#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ncs::core::application {

enum class CodeIssueStatus { Issued, Cooldown, DailyLimit, CapacityExceeded, InvalidRequest };
enum class CodeVerifyStatus { Valid, Invalid, Expired, Locked, NotFound };

struct CodeIssueResult {
    CodeIssueStatus status = CodeIssueStatus::InvalidRequest;
    std::optional<std::string> developmentCode;
    int retryAfterSec = 0;
    std::chrono::system_clock::time_point expiresAt{};
};

class VerificationCodeService final {
public:
    explicit VerificationCodeService(bool exposeDevelopmentCode);

    CodeIssueResult issue(
        std::string_view phone,
        std::string_view purpose,
        std::chrono::system_clock::time_point now);
    CodeVerifyStatus verify(
        std::string_view phone,
        std::string_view purpose,
        std::string_view code,
        std::chrono::system_clock::time_point now);
    void cleanup(std::chrono::system_clock::time_point now);
    std::size_t size() const;

private:
    struct Entry {
        std::string digest;
        std::chrono::system_clock::time_point issuedAt;
        std::chrono::system_clock::time_point expiresAt;
        int failedAttempts = 0;
        bool consumed = false;
    };

    struct DailyIssueCount {
        std::int64_t utcDay = 0;
        int count = 0;
    };

    static bool validRequest(std::string_view phone, std::string_view purpose);
    std::string digest(
        std::string_view phone,
        std::string_view purpose,
        std::string_view code) const;
    void cleanupUnlocked(std::chrono::system_clock::time_point now);
    static std::int64_t utcDay(std::chrono::system_clock::time_point now);

    bool exposeDevelopmentCode_;
    std::string pepper_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
    std::unordered_map<std::string, DailyIssueCount> dailyIssues_;
    static constexpr std::size_t maximumEntries_ = 10000;
    static constexpr std::size_t maximumDailyIssuers_ = 65536;
    static constexpr int maximumDailyIssuesPerPhone_ = 20;
};

} // namespace ncs::core::application
