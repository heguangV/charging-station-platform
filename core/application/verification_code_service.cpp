#include "core/application/verification_code_service.h"

#include "core/application/security_crypto.h"

#include <openssl/rand.h>

#include <array>
#include <iomanip>
#include <sstream>

namespace ncs::core::application {
namespace {

std::string entryKey(const std::string_view phone, const std::string_view purpose)
{
    return std::string(phone) + "\n" + std::string(purpose);
}

std::string generateCode()
{
    std::array<unsigned char, 4> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) return {};
    const unsigned int value =
        (static_cast<unsigned int>(bytes[0]) << 24)
        | (static_cast<unsigned int>(bytes[1]) << 16)
        | (static_cast<unsigned int>(bytes[2]) << 8)
        | static_cast<unsigned int>(bytes[3]);
    std::ostringstream stream;
    stream << std::setw(6) << std::setfill('0') << (value % 1000000U);
    return stream.str();
}

} // namespace

VerificationCodeService::VerificationCodeService(const bool exposeDevelopmentCode)
    : exposeDevelopmentCode_(exposeDevelopmentCode), pepper_(secureRandomToken(32))
{
}

CodeIssueResult VerificationCodeService::issue(
    const std::string_view phone,
    const std::string_view purpose,
    const std::chrono::system_clock::time_point now)
{
    if (!validRequest(phone, purpose)) return {};
    std::lock_guard lock(mutex_);
    cleanupUnlocked(now);
    const std::string key = entryKey(phone, purpose);
    const auto existing = entries_.find(key);
    if (existing != entries_.end() && now - existing->second.issuedAt < std::chrono::seconds(60)) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - existing->second.issuedAt);
        return {
            CodeIssueStatus::Cooldown,
            std::nullopt,
            60 - static_cast<int>(elapsed.count()),
            existing->second.expiresAt,
        };
    }
    const auto today = utcDay(now);
    auto daily = dailyIssues_.find(std::string(phone));
    if (daily == dailyIssues_.end()) {
        if (dailyIssues_.size() >= maximumDailyIssuers_) {
            return {CodeIssueStatus::CapacityExceeded, std::nullopt, 60, now};
        }
        daily = dailyIssues_.emplace(std::string(phone), DailyIssueCount{today, 0}).first;
    }
    if (daily->second.utcDay != today) daily->second = DailyIssueCount{today, 0};
    if (daily->second.count >= maximumDailyIssuesPerPhone_) {
        const auto nextDay = std::chrono::system_clock::time_point(
            std::chrono::hours((today + 1) * 24));
        return {
            CodeIssueStatus::DailyLimit,
            std::nullopt,
            static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                nextDay - now).count()),
            nextDay,
        };
    }
    if (entries_.size() >= maximumEntries_ && existing == entries_.end()) {
        return {CodeIssueStatus::CapacityExceeded, std::nullopt, 60, now};
    }
    const std::string code = generateCode();
    if (code.empty()) return {};
    entries_[key] = Entry{digest(phone, purpose, code), now, now + std::chrono::minutes(10)};
    ++daily->second.count;
    return {
        CodeIssueStatus::Issued,
        exposeDevelopmentCode_ ? std::optional<std::string>(code) : std::nullopt,
        0,
        now + std::chrono::minutes(10),
    };
}

CodeVerifyStatus VerificationCodeService::verify(
    const std::string_view phone,
    const std::string_view purpose,
    const std::string_view code,
    const std::chrono::system_clock::time_point now)
{
    if (!validRequest(phone, purpose) || code.size() != 6) {
        return CodeVerifyStatus::Invalid;
    }
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(entryKey(phone, purpose));
    if (found == entries_.end() || found->second.consumed) return CodeVerifyStatus::NotFound;
    Entry &entry = found->second;
    if (entry.failedAttempts >= 5) return CodeVerifyStatus::Locked;
    if (now >= entry.expiresAt) {
        entries_.erase(found);
        return CodeVerifyStatus::Expired;
    }
    if (entry.digest != digest(phone, purpose, code)) {
        ++entry.failedAttempts;
        return entry.failedAttempts >= 5 ? CodeVerifyStatus::Locked : CodeVerifyStatus::Invalid;
    }
    entries_.erase(found);
    return CodeVerifyStatus::Valid;
}

void VerificationCodeService::cleanup(const std::chrono::system_clock::time_point now)
{
    std::lock_guard lock(mutex_);
    cleanupUnlocked(now);
}

std::size_t VerificationCodeService::size() const
{
    std::lock_guard lock(mutex_);
    return entries_.size();
}

bool VerificationCodeService::validRequest(
    const std::string_view phone,
    const std::string_view purpose)
{
    const bool phoneValid = phone.size() == 11
        && phone.find_first_not_of("0123456789") == std::string_view::npos;
    return phoneValid
        && (purpose == "LOGIN" || purpose == "REGISTER" || purpose == "RESET_PASSWORD");
}

std::string VerificationCodeService::digest(
    const std::string_view phone,
    const std::string_view purpose,
    const std::string_view code) const
{
    return sha256Hex(
        pepper_ + "\n" + std::string(phone) + "\n" + std::string(purpose) + "\n"
        + std::string(code));
}

void VerificationCodeService::cleanupUnlocked(
    const std::chrono::system_clock::time_point now)
{
    for (auto iterator = entries_.begin(); iterator != entries_.end();) {
        if (iterator->second.consumed || iterator->second.expiresAt <= now) {
            iterator = entries_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    const auto today = utcDay(now);
    for (auto iterator = dailyIssues_.begin(); iterator != dailyIssues_.end();) {
        if (iterator->second.utcDay < today) {
            iterator = dailyIssues_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

std::int64_t VerificationCodeService::utcDay(
    const std::chrono::system_clock::time_point now)
{
    return std::chrono::duration_cast<std::chrono::hours>(now.time_since_epoch()).count() / 24;
}

} // namespace ncs::core::application
