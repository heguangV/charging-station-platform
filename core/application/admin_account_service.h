#pragma once

#include "core/application/admin_repository.h"
#include "core/application/security_crypto.h"
#include "core/application/service_result.h"
#include "core/application/session_manager.h"

#include <chrono>
#include <string>

namespace ncs::core::application
{

// SRS UC-A-09 管理员账号管理: an OWNER lists, creates (OPERATOR role with a
// forced first-login password change) and enables/disables admin accounts,
// while every admin changes only their own password. Mutations are audited in
// the same transaction; disabling revokes all sessions of the account and a
// password change revokes every other session of the caller.
class AdminAccountService final
{
  public:
    AdminAccountService(AdminRepository& repository, SessionManager& sessions)
        : repository_(repository), sessions_(sessions)
    {
    }

    AdminAccountPage list(const AdminAccountQuery& query);

    ServiceResult<AdminAccount> create(std::int64_t actorAdminId, std::string username,
                                       std::string password, std::string reason,
                                       std::chrono::system_clock::time_point now);

    ServiceResult<AdminAccount> updateStatus(std::int64_t actorAdminId, std::int64_t adminId,
                                             int status, std::string reason,
                                             std::int64_t expectedVersion,
                                             std::chrono::system_clock::time_point now);

    ServiceResult<AdminAccount> changeOwnPassword(std::int64_t actorAdminId, std::int64_t sessionId,
                                                  std::string currentPassword,
                                                  std::string newPassword,
                                                  std::chrono::system_clock::time_point now);

    // Shared with the one-shot OWNER bootstrap (server --bootstrap-owner):
    // 3-32 characters, ASCII alphanumeric or underscore.
    static bool validUsername(std::string_view username);

  private:
    static bool validReason(std::string_view reason);
    static std::string adminPrincipal(std::int64_t adminId);

    AdminRepository& repository_;
    SessionManager& sessions_;
    PasswordHasher passwordHasher_;
};

} // namespace ncs::core::application
