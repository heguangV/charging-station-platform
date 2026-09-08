// 短信验证码服务：下发（冷却、每日限额、容量上限）与校验；验证码以 pepper 加盐摘要存储，不落明文。
// 内存态、进程重启即失效；供用户注册/登录/改凭据流程使用；仅当 exposeDevelopmentCode
// 开启时回显开发验证码（模拟短信）。 约束：在用验证码上限 10000、每日下发号码 65536、单手机号每日
// 20 条；连续错 5 次锁定（Locked）。

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ncs::core::application
{

enum class CodeIssueStatus
{
    Issued,
    Cooldown,
    DailyLimit,
    CapacityExceeded,
    InvalidRequest
};
enum class CodeVerifyStatus
{
    Valid,
    Invalid,
    Expired,
    Locked,
    NotFound
};

struct CodeIssueResult
{
    CodeIssueStatus status = CodeIssueStatus::InvalidRequest;
    std::optional<std::string> developmentCode;
    int retryAfterSec = 0;
    std::chrono::system_clock::time_point expiresAt{};
};

class VerificationCodeService final
{
  public:
    explicit VerificationCodeService(bool exposeDevelopmentCode);

    // 生成 6 位验证码并以 pepper 加盐摘要存储（10 分钟有效）：60 秒冷却返回 Cooldown、单号每日 20
    // 条返回 DailyLimit、容量超限返回 CapacityExceeded， 均附 retryAfterSec；仅
    // exposeDevelopmentCode 开启时回显验证码（模拟短信）。
    CodeIssueResult issue(std::string_view phone, std::string_view purpose,
                          std::chrono::system_clock::time_point now);
    // 校验并一次性消费验证码：错误累计 5 次锁定（Locked），过期返回 Expired，未下发或已消费返回
    // NotFound；成功即删除条目。
    CodeVerifyStatus verify(std::string_view phone, std::string_view purpose, std::string_view code,
                            std::chrono::system_clock::time_point now);
    void cleanup(std::chrono::system_clock::time_point now);
    std::size_t size() const;

  private:
    struct Entry
    {
        std::string digest;
        std::chrono::system_clock::time_point issuedAt;
        std::chrono::system_clock::time_point expiresAt;
        int failedAttempts = 0;
        bool consumed = false;
    };

    struct DailyIssueCount
    {
        std::int64_t utcDay = 0;
        int count = 0;
    };

    static bool validRequest(std::string_view phone, std::string_view purpose);
    std::string digest(std::string_view phone, std::string_view purpose,
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
