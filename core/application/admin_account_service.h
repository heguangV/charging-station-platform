// 管理员账号管理应用服务（SRS UC-A-09）：OWNER 分页查询账号、创建 OPERATOR
// 账号、启用/停用账号，任何管理员可修改自己的密码。 依赖 AdminRepository（持久化端口）与
// SessionManager（会话吊销）；变更与其审计事件在同一事务内完成。
// 约束：新建账号为首登改密状态；停用即吊销其全部会话，改密后吊销操作者本人的其他会话。

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

    // 创建管理员账号（事务内，附审计）：用户名/密码/原因校验失败返回
    // ValidationFailed，用户名已存在返回 AlreadyExists，写入异常返回 InternalError。
    ServiceResult<AdminAccount> create(std::int64_t actorAdminId, std::string username,
                                       std::string password, std::string reason,
                                       std::chrono::system_clock::time_point now);

    // 启用/停用管理员账号：expectedVersion 乐观锁（VersionConflict），OWNER
    // 不得停用当前会话对应的自己（ValidationFailed）；停用成功即吊销该账号全部会话。
    ServiceResult<AdminAccount> updateStatus(std::int64_t actorAdminId, std::int64_t adminId,
                                             int status, std::string reason,
                                             std::int64_t expectedVersion,
                                             std::chrono::system_clock::time_point now);

    // 修改本人密码：当前密码校验失败返回 Unauthorized；成功后在事务内改密（并发改密经旧哈希比对
    // CAS）并吊销本人其他会话、保留当前会话。
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
