#pragma once

#include "core/application/security_crypto.h"
#include "core/application/service_result.h"
#include "core/application/session_manager.h"
#include "core/application/user_account_repository.h"
#include "core/application/verification_code_service.h"
#include "core/domain/error_code.h"

#include <chrono>
#include <optional>
#include <string>

namespace ncs::core::application {

struct LoginResult {
    IssuedSession session;
    UserAccount user;
};

class UserIdentityService final {
public:
    UserIdentityService(
        UserAccountRepository &accounts,
        SessionManager &sessions,
        VerificationCodeService &verificationCodes);

    ServiceResult<CodeIssueResult> issueCode(
        std::string phone,
        std::string purpose,
        std::chrono::system_clock::time_point now);
    ServiceResult<LoginResult> registerUser(
        std::string username,
        std::string phone,
        std::string password,
        std::string smsCode,
        std::string deviceId,
        std::chrono::system_clock::time_point now);
    ServiceResult<LoginResult> loginPassword(
        std::string loginName,
        std::string password,
        std::string deviceId,
        std::chrono::system_clock::time_point now);
    ServiceResult<LoginResult> loginSms(
        std::string phone,
        std::string smsCode,
        std::string deviceId,
        std::chrono::system_clock::time_point now);
    ServiceResult<UserAccount> profile(std::string_view principalId) const;
    ServiceResult<UserAccount> updateNickname(
        std::string_view principalId,
        std::string nickname,
        std::int64_t expectedVersion);
    ServiceResult<UserAccount> updateCredential(
        const AuthContext &auth,
        std::string username,
        std::optional<std::string> currentPassword,
        std::string newPassword,
        std::optional<std::string> smsCode,
        std::chrono::system_clock::time_point now);
    ServiceResult<UserAccount> updateAvatar(
        std::string_view principalId,
        AvatarData avatar);
    core::domain::ErrorCode deleteAccount(
        const AuthContext &auth,
        bool confirmed,
        std::optional<std::string> password,
        std::optional<std::string> smsCode,
        std::chrono::system_clock::time_point now);

    static bool validUsername(std::string_view username);
    static bool validPhone(std::string_view phone);
    static bool validDeviceId(std::string_view deviceId);

private:
    ServiceResult<LoginResult> issueSession(
        UserAccount account,
        std::string deviceId,
        std::chrono::system_clock::time_point now);
    static std::optional<std::int64_t> principalId(std::string_view value);

    UserAccountRepository &accounts_;
    SessionManager &sessions_;
    VerificationCodeService &verificationCodes_;
    PasswordHasher passwordHasher_;
    std::string dummyPasswordHash_;
};

} // namespace ncs::core::application
