#pragma once

#include "core/application/admin_repository.h"
#include "core/application/security_crypto.h"
#include "core/application/service_result.h"
#include "core/application/session_manager.h"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ncs::core::application {

struct AdminLoginResult {
  IssuedSession session;
  AdminAccount admin;
};

class AdminAuthService final {
public:
  AdminAuthService(AdminRepository &repository, SessionManager &sessions);

  ServiceResult<AdminLoginResult>
  login(std::string username, std::string password, std::string deviceId,
        std::chrono::system_clock::time_point now);
  ServiceResult<AdminLoginResult>
  loginDashboard(std::string username, std::string password,
                 std::string deviceId,
                 std::chrono::system_clock::time_point now);
  ServiceResult<std::int64_t>
  reauthenticate(const AuthContext &auth, std::string_view password,
                 std::chrono::system_clock::time_point now);

  static std::optional<std::int64_t> principalId(std::string_view principal);

private:
  struct AttemptState {
    int failures = 0;
    std::chrono::system_clock::time_point lockedUntil{};
    std::chrono::system_clock::time_point lastAttempt{};
  };

  bool locked(std::string_view username,
              std::chrono::system_clock::time_point now);
  bool recordFailure(std::string username,
                     std::chrono::system_clock::time_point now);
  void clearFailures(std::string_view username);
  void cleanupAttempts(std::chrono::system_clock::time_point now);
  ServiceResult<AdminLoginResult>
  loginAs(std::string username, std::string password, std::string deviceId,
          TokenKind tokenKind, std::chrono::system_clock::time_point now);

  AdminRepository &repository_;
  SessionManager &sessions_;
  PasswordHasher passwordHasher_;
  std::string dummyPasswordHash_;
  std::mutex attemptsMutex_;
  std::unordered_map<std::string, AttemptState> attempts_;
  static constexpr std::size_t maximumAttempts = 65536;
};

} // namespace ncs::core::application
