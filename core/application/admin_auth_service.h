// 管理员登录应用服务：密码登录（含大屏登录）与敏感操作再认证，principal 采用 "admin:<id>" 形式。
// 依赖 AdminRepository（账号查询）与 SessionManager（签发会话）；账号未命中时用 dummy
// 哈希做恒时校验，防用户名探测。 约束：内存态登录失败锁定（30
// 秒）与容量上限（65536），进程重启即清零。

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

namespace ncs::core::application
{

struct AdminLoginResult
{
    IssuedSession session;
    AdminAccount admin;
};

class AdminAuthService final
{
  public:
    AdminAuthService(AdminRepository& repository, SessionManager& sessions);

    // 管理端密码登录：账号未命中也执行 dummy 恒时校验防用户名探测；连续失败 5 次锁定 30
    // 秒（RateLimited），成功签发 8 小时 Administrator 会话。
    ServiceResult<AdminLoginResult> login(std::string username, std::string password,
                                          std::string deviceId,
                                          std::chrono::system_clock::time_point now);
    // 大屏密码登录：允许 Operator/Owner/Viewer 角色，签发 8 小时 Dashboard 会话（30
    // 分钟无活动将被回收）；失败语义同 login。
    ServiceResult<AdminLoginResult> loginDashboard(std::string username, std::string password,
                                                   std::string deviceId,
                                                   std::chrono::system_clock::time_point now);
    // 敏感操作再认证：校验当前会话归属管理员的密码，成功在会话上标记再认证时间并返回有效期截止（now+15
    // 分钟）的 UTC 秒；失败返回 Unauthorized。
    ServiceResult<std::int64_t> reauthenticate(const AuthContext& auth, std::string_view password,
                                               std::chrono::system_clock::time_point now);

    static std::optional<std::int64_t> principalId(std::string_view principal);

  private:
    struct AttemptState
    {
        int failures = 0;
        std::chrono::system_clock::time_point lockedUntil{};
        std::chrono::system_clock::time_point lastAttempt{};
    };

    bool locked(std::string_view username, std::chrono::system_clock::time_point now);
    bool recordFailure(std::string username, std::chrono::system_clock::time_point now);
    void clearFailures(std::string_view username);
    void cleanupAttempts(std::chrono::system_clock::time_point now);
    ServiceResult<AdminLoginResult> loginAs(std::string username, std::string password,
                                            std::string deviceId, TokenKind tokenKind,
                                            std::chrono::system_clock::time_point now);

    AdminRepository& repository_;
    SessionManager& sessions_;
    PasswordHasher passwordHasher_;
    std::string dummyPasswordHash_;
    std::mutex attemptsMutex_;
    std::unordered_map<std::string, AttemptState> attempts_;
    static constexpr std::size_t maximumAttempts = 65536;
};

} // namespace ncs::core::application
