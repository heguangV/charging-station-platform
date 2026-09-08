// 内存会话管理器：签发/校验 Bearer 令牌（仅存 SHA-256
// 摘要）、按会话或主体吊销、标记再认证并清理过期会话。 支撑用户、管理员、大屏与 ML
// 任务四类令牌（TokenKind/Role）；吊销经 RevocationObserver 通知 EventHub 下发 session.revoked。
// 约束：状态仅在进程内存（重启即失效），总量上限 16384；吊销观察者在无锁状态下回调。

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ncs::core::application
{

enum class TokenKind
{
    User,
    Administrator,
    Dashboard,
    MlTask
};
enum class Role
{
    User,
    Operator,
    Owner,
    Viewer,
    MlWorker,
    MlTrainer,
    MlPredictor
};

struct AuthContext
{
    std::int64_t sessionId = 0;
    std::string principalId;
    std::string deviceId;
    TokenKind tokenKind = TokenKind::User;
    std::vector<Role> roles;
    std::chrono::system_clock::time_point expiresAt;
    std::optional<std::chrono::system_clock::time_point> reauthenticatedAt;
};

struct IssuedSession
{
    std::string accessToken;
    AuthContext context;
};

struct SessionView
{
    std::int64_t sessionId = 0;
    std::string deviceId;
    std::chrono::system_clock::time_point createdAt;
    std::chrono::system_clock::time_point lastSeenAt;
    std::chrono::system_clock::time_point expiresAt;
};

using RevocationObserver =
    std::function<void(std::int64_t sessionId, std::string_view principalId)>;

class SessionManager final
{
  public:
    // Installed once before the server accepts work. The observer is invoked
    // with NO SessionManager lock held; exceptions from it are swallowed.
    void setRevocationObserver(RevocationObserver observer);
    // 签发会话：令牌仅存 SHA-256 摘要；同 TokenKind 会话按限额（用户 3/管理员 2/大屏 2/ML
    // 1）控制，同设备旧会话被替换并通知吊销； 超出限额/总量 16384 或 lifetime 越界（用户 >30
    // 天、大屏 >8 小时）返回 std::nullopt。
    std::optional<IssuedSession> issue(std::string principalId, std::string deviceId,
                                       TokenKind tokenKind, std::vector<Role> roles,
                                       std::chrono::system_clock::time_point now,
                                       std::chrono::seconds lifetime);

    // 校验 Bearer 令牌：按摘要查找会话，已吊销/过期即清除并返回 std::nullopt；Dashboard 会话 30
    // 分钟无活动强制失效；成功刷新 lastSeen 并返回上下文。
    std::optional<AuthContext> authenticate(std::string_view rawToken,
                                            std::chrono::system_clock::time_point now);
    // 按会话 ID 吊销并通知观察者；会话不存在时视为成功（幂等，返回 true）。
    bool revoke(std::int64_t sessionId);
    // 吊销指定会话，但仅当其确属该 principalId 时生效；返回值恒为
    // true，是否真正吊销以观察者通知为准。
    bool revokeForPrincipal(std::string_view principalId, std::int64_t sessionId);
    // 吊销该主体（principalId）的全部会话并逐个通知，返回吊销数量；无会话时返回 0。
    std::size_t revokePrincipal(std::string_view principalId);
    // 吊销该主体除 currentSessionId
    // 外的全部会话并通知，返回吊销数量；改密/改凭据后用于保留当前会话。
    std::size_t revokeOtherSessions(std::string_view principalId, std::int64_t currentSessionId);
    std::vector<SessionView> activeSessions(std::string_view principalId,
                                            std::chrono::system_clock::time_point now) const;
    void cleanup(std::chrono::system_clock::time_point now);
    std::size_t size() const;
    // 在存活会话上标记再认证时间；会话不存在/已吊销/已过期返回 false。
    bool markReauthenticated(std::int64_t sessionId, std::chrono::system_clock::time_point now);
    // 判断会话是否在 window（默认 15 分钟）内完成过再认证，供敏感操作放行判断。
    bool hasRecentReauthentication(const AuthContext& context,
                                   std::chrono::system_clock::time_point now,
                                   std::chrono::minutes window = std::chrono::minutes(15)) const;

    static std::optional<std::string_view> parseBearer(std::string_view authorization);
    static bool allowsPath(TokenKind tokenKind, std::string_view path);
    static bool hasRole(const AuthContext& context, Role role);

  private:
    struct StoredSession
    {
        AuthContext context;
        std::string tokenDigest;
        std::chrono::system_clock::time_point lastSeenAt;
        std::chrono::system_clock::time_point createdAt;
        bool revoked = false;
    };

    static std::size_t sessionLimit(TokenKind tokenKind);
    void notifyRevocation(std::int64_t sessionId, const std::string& principalId);
    void cleanupUnlocked(std::chrono::system_clock::time_point now);
    void eraseSessionUnlocked(std::int64_t sessionId);
    void removeFromPrincipalIndexUnlocked(const std::string& principalId, std::int64_t sessionId);

    mutable std::mutex mutex_;
    RevocationObserver revocationObserver_;
    std::unordered_map<std::int64_t, StoredSession> sessionsById_;
    std::unordered_map<std::string, std::int64_t> sessionIdByDigest_;
    std::unordered_map<std::string, std::vector<std::int64_t>> sessionIdsByPrincipal_;
    std::int64_t nextSessionId_ = 1;
    static constexpr std::size_t maximumSessions_ = 16384;
};

} // namespace ncs::core::application
