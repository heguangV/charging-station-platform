#include "core/application/admin_auth_service.h"

#include <algorithm>
#include <charconv>

namespace ncs::core::application {

AdminAuthService::AdminAuthService(AdminRepository &repository,
                                   SessionManager &sessions)
    : repository_(repository), sessions_(sessions),
      dummyPasswordHash_(passwordHasher_.hash("ncs-dummy-admin-password")) {}

ServiceResult<AdminLoginResult> AdminAuthService::login(
    std::string username, std::string password, std::string deviceId,
    const std::chrono::system_clock::time_point now) {
  return loginAs(std::move(username), std::move(password), std::move(deviceId),
                 TokenKind::Administrator, now);
}

ServiceResult<AdminLoginResult> AdminAuthService::loginDashboard(
    std::string username, std::string password, std::string deviceId,
    const std::chrono::system_clock::time_point now) {
  return loginAs(std::move(username), std::move(password), std::move(deviceId),
                 TokenKind::Dashboard, now);
}

ServiceResult<AdminLoginResult> AdminAuthService::loginAs(
    std::string username, std::string password, std::string deviceId,
    const TokenKind tokenKind,
    const std::chrono::system_clock::time_point now) {
  if (username.empty() || username.size() > 64 || password.empty() ||
      password.size() > 128 || deviceId.empty() || deviceId.size() > 128) {
    return {core::domain::ErrorCode::Unauthorized, std::nullopt};
  }
  if (locked(username, now))
    return {core::domain::ErrorCode::RateLimited, std::nullopt};

  auto account = repository_.findAdminByUsername(username);
  const std::string &hash = account ? account->passwordHash : dummyPasswordHash_;
  if (!passwordHasher_.verify(password, hash) || !account ||
      account->status != 1) {
    return {recordFailure(std::move(username), now)
                ? core::domain::ErrorCode::RateLimited
                : core::domain::ErrorCode::Unauthorized,
            std::nullopt};
  }
  const bool permitted = std::any_of(
      account->roles.begin(), account->roles.end(),
      [tokenKind](const Role role) {
        if (tokenKind == TokenKind::Dashboard)
          return role == Role::Operator || role == Role::Owner ||
                 role == Role::Viewer;
        return role == Role::Operator || role == Role::Owner;
      });
  if (!permitted) {
    return {core::domain::ErrorCode::Unauthorized, std::nullopt};
  }
  clearFailures(username);
  auto session = sessions_.issue(
      "admin:" + std::to_string(account->id), std::move(deviceId),
      tokenKind, account->roles, now, std::chrono::hours(8));
  if (!session)
    return {core::domain::ErrorCode::RateLimited, std::nullopt};
  return {core::domain::ErrorCode::Ok,
          AdminLoginResult{std::move(*session), std::move(*account)}};
}

ServiceResult<std::int64_t> AdminAuthService::reauthenticate(
    const AuthContext &auth, const std::string_view password,
    const std::chrono::system_clock::time_point now) {
  const auto id = principalId(auth.principalId);
  const auto account = id ? repository_.findAdminById(*id) : std::nullopt;
  if (!account || account->status != 1 || password.empty() ||
      !passwordHasher_.verify(password, account->passwordHash)) {
    return {core::domain::ErrorCode::Unauthorized, std::nullopt};
  }
  if (!sessions_.markReauthenticated(auth.sessionId, now))
    return {core::domain::ErrorCode::Unauthorized, std::nullopt};
  return {core::domain::ErrorCode::Ok,
          std::chrono::duration_cast<std::chrono::seconds>(
              (now + std::chrono::minutes(15)).time_since_epoch())
              .count()};
}

std::optional<std::int64_t>
AdminAuthService::principalId(const std::string_view principal) {
  constexpr std::string_view prefix = "admin:";
  if (principal.size() <= prefix.size() ||
      principal.substr(0, prefix.size()) != prefix)
    return std::nullopt;
  const auto numeric = principal.substr(prefix.size());
  std::int64_t value = 0;
  const auto parsed =
      std::from_chars(numeric.data(), numeric.data() + numeric.size(), value);
  return parsed.ec == std::errc{} &&
                 parsed.ptr == numeric.data() + numeric.size() && value > 0
             ? std::optional<std::int64_t>(value)
             : std::nullopt;
}

bool AdminAuthService::locked(const std::string_view username,
                              const std::chrono::system_clock::time_point now) {
  std::lock_guard lock(attemptsMutex_);
  cleanupAttempts(now);
  const auto found = attempts_.find(std::string(username));
  return found != attempts_.end() && found->second.lockedUntil > now;
}

bool AdminAuthService::recordFailure(
    std::string username, const std::chrono::system_clock::time_point now) {
  std::lock_guard lock(attemptsMutex_);
  cleanupAttempts(now);
  if (attempts_.size() >= maximumAttempts && !attempts_.count(username))
    return true;
  auto &state = attempts_[std::move(username)];
  state.lastAttempt = now;
  ++state.failures;
  if (state.failures >= 5) {
    state.failures = 0;
    state.lockedUntil = now + std::chrono::seconds(30);
    return true;
  }
  return false;
}

void AdminAuthService::clearFailures(const std::string_view username) {
  std::lock_guard lock(attemptsMutex_);
  attempts_.erase(std::string(username));
}

void AdminAuthService::cleanupAttempts(
    const std::chrono::system_clock::time_point now) {
  for (auto iterator = attempts_.begin(); iterator != attempts_.end();) {
    if (iterator->second.lockedUntil <= now &&
        now - iterator->second.lastAttempt > std::chrono::minutes(10)) {
      iterator = attempts_.erase(iterator);
    } else {
      ++iterator;
    }
  }
}

} // namespace ncs::core::application
