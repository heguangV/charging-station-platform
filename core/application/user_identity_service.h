// 用户身份应用服务：短信验证码下发、注册、密码登录、短信登录、资料/昵称/凭据/头像维护与账号注销（匿名化）。
// 依赖 UserAccountRepository、SessionManager（签发会话）与 VerificationCodeService；PasswordHasher
// 负责口令哈希与未命中账号的 dummy 恒时比较。
// 约束：注销需显式确认并校验密码或短信码；登录名/手机号冲突返回 AlreadyExists；登录成功签发新会话。

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

namespace ncs::core::application
{

struct LoginResult
{
    IssuedSession session;
    UserAccount user;
};

class UserIdentityService final
{
  public:
    UserIdentityService(UserAccountRepository& accounts, SessionManager& sessions,
                        VerificationCodeService& verificationCodes);

    // 下发短信验证码：响应不透露手机号是否已注册；命中 60 秒冷却/单号每日 20 条/容量上限时返回
    // RateLimited（载荷带状态与重试间隔），号码非法返回 InvalidArgument。
    ServiceResult<CodeIssueResult> issueCode(std::string phone, std::string purpose,
                                             std::chrono::system_clock::time_point now);
    // 用户名密码注册：先校验 REGISTER 验证码（CodeInvalid/CodeExpired），登录名或手机号已存在返回
    // AlreadyExists；成功即签发会话。
    ServiceResult<LoginResult> registerUser(std::string username, std::string phone,
                                            std::string password, std::string smsCode,
                                            std::string deviceId,
                                            std::chrono::system_clock::time_point now);
    // 密码登录：账号未命中使用 dummy 哈希做恒时比较（统一返回 Unauthorized）；账号冻结返回
    // UserFrozen；命中低迭代旧哈希时尽力透明重哈希。
    ServiceResult<LoginResult> loginPassword(std::string loginName, std::string password,
                                             std::string deviceId,
                                             std::chrono::system_clock::time_point now);
    // 短信验证码登录：验证码通过后手机号未注册则自动建号（昵称按手机尾号生成）；账号冻结返回
    // UserFrozen，成功签发 30 天会话。
    ServiceResult<LoginResult> loginSms(std::string phone, std::string smsCode,
                                        std::string deviceId,
                                        std::chrono::system_clock::time_point now);
    ServiceResult<UserAccount> profile(std::string_view principalId) const;
    // 更新昵称（≤20 个 UTF-8 码点、禁止纯空白与控制字符）：expectedVersion 乐观锁不符返回
    // VersionConflict，用户不存在返回 NotFound。
    ServiceResult<UserAccount> updateNickname(std::string_view principalId, std::string nickname,
                                              std::int64_t expectedVersion);
    // 修改登录名与密码：凭既有密码或短信验证码二选一核身（失败返回 Unauthorized）；新登录名冲突返回
    // AlreadyExists；成功后吊销本人其他会话。
    ServiceResult<UserAccount> updateCredential(const AuthContext& auth, std::string username,
                                                std::optional<std::string> currentPassword,
                                                std::string newPassword,
                                                std::optional<std::string> smsCode,
                                                std::chrono::system_clock::time_point now);
    ServiceResult<UserAccount> updateAvatar(std::string_view principalId, AvatarData avatar);
    // 注销账号（匿名化）：须显式 confirmed 并通过密码或短信码核身（否则
    // ValidationFailed/Unauthorized），存在进行中充电流程时拒绝（ActiveFlowExists）；
    // 成功后吊销全部会话；直接返回最终错误码。
    core::domain::ErrorCode deleteAccount(const AuthContext& auth, bool confirmed,
                                          std::optional<std::string> password,
                                          std::optional<std::string> smsCode,
                                          std::chrono::system_clock::time_point now);

    static bool validUsername(std::string_view username);
    static bool validPhone(std::string_view phone);
    static bool validDeviceId(std::string_view deviceId);

  private:
    ServiceResult<LoginResult> issueSession(UserAccount account, std::string deviceId,
                                            std::chrono::system_clock::time_point now);
    static std::optional<std::int64_t> principalId(std::string_view value);

    UserAccountRepository& accounts_;
    SessionManager& sessions_;
    VerificationCodeService& verificationCodes_;
    PasswordHasher passwordHasher_;
    std::string dummyPasswordHash_;
};

} // namespace ncs::core::application
