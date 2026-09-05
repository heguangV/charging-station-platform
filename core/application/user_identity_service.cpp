#include "core/application/user_identity_service.h"

#include "core/application/security_crypto.h"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace ncs::core::application {
namespace {

core::domain::ErrorCode codeError(const CodeVerifyStatus status) {
  return status == CodeVerifyStatus::Expired
             ? core::domain::ErrorCode::CodeExpired
             : core::domain::ErrorCode::CodeInvalid;
}

bool blank(const std::string_view value) {
  return std::all_of(value.begin(), value.end(),
                     [](const unsigned char character) {
                       return std::isspace(character) != 0;
                     });
}

std::size_t utf8CodePointCount(const std::string_view value) {
  return static_cast<std::size_t>(std::count_if(
      value.begin(), value.end(), [](const unsigned char character) {
        return (character & 0xc0U) != 0x80U;
      }));
}

bool containsControlCharacter(const std::string_view value) {
  for (std::size_t index = 0; index < value.size(); ++index) {
    const auto byte = static_cast<unsigned char>(value[index]);
    if (byte < 0x20 || byte == 0x7F)
      return true;
    if (byte == 0xC2 && index + 1 < value.size() &&
        (static_cast<unsigned char>(value[index + 1]) & 0xFEU) == 0x80U) {
      return true; // C1 range U+0080..U+009F
    }
  }
  return false;
}

} // namespace

UserIdentityService::UserIdentityService(
    UserAccountRepository &accounts, SessionManager &sessions,
    VerificationCodeService &verificationCodes)
    : accounts_(accounts), sessions_(sessions),
      verificationCodes_(verificationCodes),
      dummyPasswordHash_(passwordHasher_.hash("ncs-dummy-password")) {}

ServiceResult<CodeIssueResult> UserIdentityService::issueCode(
    std::string phone, std::string purpose,
    const std::chrono::system_clock::time_point now) {
  if (!validPhone(phone))
    return {core::domain::ErrorCode::InvalidArgument, std::nullopt};
  // No existence probe here: the response must not reveal whether the phone
  // is registered, mirroring the unified login failure rule.
  const CodeIssueResult result = verificationCodes_.issue(phone, purpose, now);
  if (result.status == CodeIssueStatus::InvalidRequest) {
    return {core::domain::ErrorCode::InvalidArgument, std::nullopt};
  }
  if (result.status == CodeIssueStatus::Cooldown ||
      result.status == CodeIssueStatus::DailyLimit ||
      result.status == CodeIssueStatus::CapacityExceeded) {
    return {core::domain::ErrorCode::RateLimited, result};
  }
  return {core::domain::ErrorCode::Ok, result};
}

ServiceResult<LoginResult> UserIdentityService::registerUser(
    std::string username, std::string phone, std::string password,
    std::string smsCode, std::string deviceId,
    const std::chrono::system_clock::time_point now) {
  if (!validUsername(username) || !validPhone(phone) ||
      !validDeviceId(deviceId) || password.size() < 10 ||
      password.size() > 128) {
    return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
  }
  if (accounts_.findByLoginName(username) || accounts_.findByPhone(phone)) {
    return {core::domain::ErrorCode::AlreadyExists, std::nullopt};
  }
  const auto verified =
      verificationCodes_.verify(phone, "REGISTER", smsCode, now);
  if (verified != CodeVerifyStatus::Valid)
    return {codeError(verified), std::nullopt};
  UserAccount account;
  account.username = std::move(username);
  account.phone = std::move(phone);
  account.nickname = "用户" + account.phone.substr(7);
  account.passwordHash = passwordHasher_.hash(password);
  account.registeredAt =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();
  const auto created = accounts_.create(account);
  if (created == AccountWriteResult::UsernameExists ||
      created == AccountWriteResult::PhoneExists) {
    return {core::domain::ErrorCode::AlreadyExists, std::nullopt};
  }
  return issueSession(std::move(account), std::move(deviceId), now);
}

ServiceResult<LoginResult> UserIdentityService::loginPassword(
    std::string loginName, std::string password, std::string deviceId,
    const std::chrono::system_clock::time_point now) {
  if (!validDeviceId(deviceId) || password.empty()) {
    return {core::domain::ErrorCode::Unauthorized, std::nullopt};
  }
  auto account = accounts_.findByLoginName(loginName);
  const std::string &hash = account && account->passwordHash
                                ? *account->passwordHash
                                : dummyPasswordHash_;
  if (!passwordHasher_.verify(password, hash) || !account ||
      !account->passwordHash) {
    return {core::domain::ErrorCode::Unauthorized, std::nullopt};
  }
  if (account->status != 1)
    return {core::domain::ErrorCode::UserFrozen, std::nullopt};
  if (PasswordHasher::needsRehash(*account->passwordHash)) {
    // Best effort: a concurrent credential change loses the race via the
    // digest CAS and simply keeps the newer hash.
    accounts_.replacePasswordHash(account->id, *account->passwordHash,
                                  passwordHasher_.hash(password));
  }
  return issueSession(std::move(*account), std::move(deviceId), now);
}

ServiceResult<LoginResult>
UserIdentityService::loginSms(std::string phone, std::string smsCode,
                              std::string deviceId,
                              const std::chrono::system_clock::time_point now) {
  if (!validPhone(phone) || !validDeviceId(deviceId)) {
    return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
  }
  const auto verified = verificationCodes_.verify(phone, "LOGIN", smsCode, now);
  if (verified != CodeVerifyStatus::Valid)
    return {codeError(verified), std::nullopt};
  auto account = accounts_.findByPhone(phone);
  if (!account) {
    UserAccount created;
    created.phone = phone;
    created.username = "user_" + sha256Hex(phone).substr(0, 16);
    created.nickname = "用户" + phone.substr(7);
    created.registeredAt =
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
            .count();
    if (accounts_.create(created) != AccountWriteResult::Success) {
      return {core::domain::ErrorCode::AlreadyExists, std::nullopt};
    }
    account = std::move(created);
  }
  if (account->status != 1)
    return {core::domain::ErrorCode::UserFrozen, std::nullopt};
  return issueSession(std::move(*account), std::move(deviceId), now);
}

ServiceResult<UserAccount>
UserIdentityService::profile(const std::string_view principal) const {
  const auto id = principalId(principal);
  const auto account = id ? accounts_.findById(*id) : std::nullopt;
  return account
             ? ServiceResult<UserAccount>{core::domain::ErrorCode::Ok, account}
             : ServiceResult<UserAccount>{core::domain::ErrorCode::NotFound,
                                          std::nullopt};
}

ServiceResult<UserAccount>
UserIdentityService::updateNickname(const std::string_view principal,
                                    std::string nickname,
                                    const std::int64_t expectedVersion) {
  const auto id = principalId(principal);
  if (!id || nickname.empty() || utf8CodePointCount(nickname) > 20 ||
      blank(nickname) || containsControlCharacter(nickname)) {
    return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
  }
  UserAccount updated;
  const auto result = accounts_.updateNickname(*id, expectedVersion,
                                               std::move(nickname), updated);
  if (result == AccountWriteResult::VersionConflict) {
    return {core::domain::ErrorCode::VersionConflict, std::nullopt};
  }
  if (result != AccountWriteResult::Success) {
    return {core::domain::ErrorCode::NotFound, std::nullopt};
  }
  return {core::domain::ErrorCode::Ok, std::move(updated)};
}

ServiceResult<UserAccount> UserIdentityService::updateCredential(
    const AuthContext &auth, std::string username,
    std::optional<std::string> currentPassword, std::string newPassword,
    std::optional<std::string> smsCode,
    const std::chrono::system_clock::time_point now) {
  auto current = profile(auth.principalId);
  if (!current.ok() || !validUsername(username) || newPassword.size() < 10 ||
      newPassword.size() > 128) {
    return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
  }
  bool verified = false;
  if (current.value->passwordHash && currentPassword) {
    verified =
        passwordHasher_.verify(*currentPassword, *current.value->passwordHash);
  } else if (smsCode) {
    verified =
        verificationCodes_.verify(current.value->phone, "RESET_PASSWORD",
                                  *smsCode, now) == CodeVerifyStatus::Valid;
  }
  if (!verified)
    return {core::domain::ErrorCode::Unauthorized, std::nullopt};
  UserAccount updated;
  const auto result =
      accounts_.updateCredential(current.value->id, std::move(username),
                                 passwordHasher_.hash(newPassword), updated);
  if (result == AccountWriteResult::UsernameExists) {
    return {core::domain::ErrorCode::AlreadyExists, std::nullopt};
  }
  if (result != AccountWriteResult::Success) {
    return {core::domain::ErrorCode::NotFound, std::nullopt};
  }
  sessions_.revokeOtherSessions(auth.principalId, auth.sessionId);
  return {core::domain::ErrorCode::Ok, std::move(updated)};
}

ServiceResult<UserAccount>
UserIdentityService::updateAvatar(const std::string_view principal,
                                  AvatarData avatar) {
  const auto id = principalId(principal);
  UserAccount updated;
  if (!id || accounts_.updateAvatar(*id, std::move(avatar), updated) !=
                 AccountWriteResult::Success) {
    return {core::domain::ErrorCode::NotFound, std::nullopt};
  }
  return {core::domain::ErrorCode::Ok, std::move(updated)};
}

core::domain::ErrorCode UserIdentityService::deleteAccount(
    const AuthContext &auth, const bool confirmed,
    std::optional<std::string> password, std::optional<std::string> smsCode,
    const std::chrono::system_clock::time_point now) {
  auto current = profile(auth.principalId);
  if (!confirmed || !current.ok())
    return core::domain::ErrorCode::ValidationFailed;
  if (current.value->hasActiveFlow)
    return core::domain::ErrorCode::ActiveFlowExists;
  bool verified =
      current.value->passwordHash && password &&
      passwordHasher_.verify(*password, *current.value->passwordHash);
  if (!verified && smsCode) {
    verified =
        verificationCodes_.verify(current.value->phone, "RESET_PASSWORD",
                                  *smsCode, now) == CodeVerifyStatus::Valid;
  }
  if (!verified)
    return core::domain::ErrorCode::Unauthorized;
  UserAccount anonymized;
  const auto result = accounts_.anonymize(current.value->id, anonymized);
  if (result == AccountWriteResult::ActiveFlowExists)
    return core::domain::ErrorCode::ActiveFlowExists;
  if (result != AccountWriteResult::Success) {
    return core::domain::ErrorCode::NotFound;
  }
  sessions_.revokePrincipal(auth.principalId);
  return core::domain::ErrorCode::Ok;
}

bool UserIdentityService::validUsername(const std::string_view username) {
  // A digits-only username would collide with a phone number in
  // findByLoginName, which matches username or phone.
  return username.size() >= 3 && username.size() <= 32 &&
         username.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnop"
                                    "qrstuvwxyz0123456789_") ==
             std::string_view::npos &&
         username.find_first_not_of("0123456789") != std::string_view::npos;
}

bool UserIdentityService::validPhone(const std::string_view phone) {
  return phone.size() == 11 && phone.front() == '1' &&
         phone.find_first_not_of("0123456789") == std::string_view::npos;
}

bool UserIdentityService::validDeviceId(const std::string_view deviceId) {
  return !deviceId.empty() && deviceId.size() <= 128;
}

ServiceResult<LoginResult> UserIdentityService::issueSession(
    UserAccount account, std::string deviceId,
    const std::chrono::system_clock::time_point now) {
  auto session = sessions_.issue(
      "user:" + std::to_string(account.id), std::move(deviceId), TokenKind::User,
      {Role::User}, now, std::chrono::hours(24 * 30));
  if (!session)
    return {core::domain::ErrorCode::Unauthorized, std::nullopt};
  return {core::domain::ErrorCode::Ok,
          LoginResult{std::move(*session), std::move(account)}};
}

std::optional<std::int64_t>
UserIdentityService::principalId(const std::string_view value) {
  constexpr std::string_view prefix = "user:";
  if (value.size() <= prefix.size() || value.substr(0, prefix.size()) != prefix)
    return std::nullopt;
  const std::string_view numeric = value.substr(prefix.size());
  std::int64_t id = 0;
  const auto parsed = std::from_chars(numeric.data(),
                                      numeric.data() + numeric.size(), id);
  if (parsed.ec != std::errc{} ||
      parsed.ptr != numeric.data() + numeric.size() ||
      id < 1) {
    return std::nullopt;
  }
  return id;
}

} // namespace ncs::core::application
