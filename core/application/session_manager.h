#pragma once

#include <chrono>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ncs::core::application {

enum class TokenKind { User, Administrator, Dashboard, MlTask };
enum class Role { User, Operator, Owner, Viewer, MlWorker, MlTrainer, MlPredictor };

struct AuthContext {
    std::int64_t sessionId = 0;
    std::string principalId;
    std::string deviceId;
    TokenKind tokenKind = TokenKind::User;
    std::vector<Role> roles;
    std::chrono::system_clock::time_point expiresAt;
    std::optional<std::chrono::system_clock::time_point> reauthenticatedAt;
};

struct IssuedSession {
    std::string accessToken;
    AuthContext context;
};

struct SessionView {
    std::int64_t sessionId = 0;
    std::string deviceId;
    std::chrono::system_clock::time_point createdAt;
    std::chrono::system_clock::time_point lastSeenAt;
    std::chrono::system_clock::time_point expiresAt;
};

using RevocationObserver = std::function<void(
    std::int64_t sessionId,
    std::string_view principalId)>;

class SessionManager final {
public:
    // Installed once before the server accepts work. The observer is invoked
    // with NO SessionManager lock held; exceptions from it are swallowed.
    void setRevocationObserver(RevocationObserver observer);
    std::optional<IssuedSession> issue(
        std::string principalId,
        std::string deviceId,
        TokenKind tokenKind,
        std::vector<Role> roles,
        std::chrono::system_clock::time_point now,
        std::chrono::seconds lifetime);

    std::optional<AuthContext> authenticate(
        std::string_view rawToken,
        std::chrono::system_clock::time_point now);
    bool revoke(std::int64_t sessionId);
    bool revokeForPrincipal(std::string_view principalId, std::int64_t sessionId);
    std::size_t revokePrincipal(std::string_view principalId);
    std::size_t revokeOtherSessions(
        std::string_view principalId,
        std::int64_t currentSessionId);
    std::vector<SessionView> activeSessions(
        std::string_view principalId,
        std::chrono::system_clock::time_point now) const;
    void cleanup(std::chrono::system_clock::time_point now);
    std::size_t size() const;
    bool markReauthenticated(
        std::int64_t sessionId,
        std::chrono::system_clock::time_point now);
    bool hasRecentReauthentication(
        const AuthContext &context,
        std::chrono::system_clock::time_point now,
        std::chrono::minutes window = std::chrono::minutes(15)) const;

    static std::optional<std::string_view> parseBearer(std::string_view authorization);
    static bool allowsPath(TokenKind tokenKind, std::string_view path);
    static bool hasRole(const AuthContext &context, Role role);

private:
    struct StoredSession {
        AuthContext context;
        std::string tokenDigest;
        std::chrono::system_clock::time_point lastSeenAt;
        std::chrono::system_clock::time_point createdAt;
        bool revoked = false;
    };

    static std::size_t sessionLimit(TokenKind tokenKind);
    void notifyRevocation(std::int64_t sessionId, const std::string &principalId);
    void cleanupUnlocked(std::chrono::system_clock::time_point now);
    void eraseSessionUnlocked(std::int64_t sessionId);
    void removeFromPrincipalIndexUnlocked(
        const std::string &principalId,
        std::int64_t sessionId);

    mutable std::mutex mutex_;
    RevocationObserver revocationObserver_;
    std::unordered_map<std::int64_t, StoredSession> sessionsById_;
    std::unordered_map<std::string, std::int64_t> sessionIdByDigest_;
    std::unordered_map<std::string, std::vector<std::int64_t>> sessionIdsByPrincipal_;
    std::int64_t nextSessionId_ = 1;
    static constexpr std::size_t maximumSessions_ = 16384;
};

} // namespace ncs::core::application
