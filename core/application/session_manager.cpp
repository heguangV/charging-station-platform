#include "core/application/session_manager.h"

#include "core/application/security_crypto.h"

#include <algorithm>

namespace ncs::core::application {
namespace {

bool startsWith(const std::string_view value, const std::string_view prefix)
{
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

} // namespace

void SessionManager::setRevocationObserver(RevocationObserver observer)
{
    std::lock_guard lock(mutex_);
    revocationObserver_ = std::move(observer);
}

void SessionManager::notifyRevocation(
    const std::int64_t sessionId,
    const std::string &principalId)
{
    if (!revocationObserver_) return;
    try {
        revocationObserver_(sessionId, principalId);
    } catch (...) {
        // Revocation observers are best-effort; never let them break
        // session management.
    }
}

std::optional<IssuedSession> SessionManager::issue(
    std::string principalId,
    std::string deviceId,
    const TokenKind tokenKind,
    std::vector<Role> roles,
    const std::chrono::system_clock::time_point now,
    const std::chrono::seconds lifetime)
{
    if (principalId.empty() || deviceId.empty() || lifetime <= std::chrono::seconds::zero()) {
        return std::nullopt;
    }
    if ((tokenKind == TokenKind::User && lifetime > std::chrono::hours(24 * 30))
        || (tokenKind == TokenKind::Dashboard && lifetime > std::chrono::hours(8))) {
        return std::nullopt;
    }
    std::vector<std::int64_t> replacedSessions;
    IssuedSession issued;
    {
        std::lock_guard lock(mutex_);
        cleanupUnlocked(now);
        std::size_t activeCount = 0;
        const auto principalSessions = sessionIdsByPrincipal_.find(principalId);
        if (principalSessions != sessionIdsByPrincipal_.end()) {
            for (const auto id : principalSessions->second) {
                const auto found = sessionsById_.find(id);
                if (found == sessionsById_.end()) continue;
                if (found->second.context.tokenKind != tokenKind) continue;
                if (found->second.context.deviceId == deviceId) {
                    replacedSessions.push_back(id);
                } else {
                    ++activeCount;
                }
            }
        }
        if (activeCount >= sessionLimit(tokenKind)) return std::nullopt;
        // Capacity is evaluated before replacement erasure: a failed issue
        // must not revoke the very session it intended to replace.
        if (sessionsById_.size() + 1
            > maximumSessions_ + replacedSessions.size()) {
            return std::nullopt;
        }
        for (const auto id : replacedSessions) eraseSessionUnlocked(id);

        const std::string rawToken = secureRandomToken(32);
        const std::int64_t sessionId = nextSessionId_++;
        AuthContext context{
            sessionId,
            std::move(principalId),
            std::move(deviceId),
            tokenKind,
            std::move(roles),
            now + lifetime,
            std::nullopt,
        };
        StoredSession stored{context, sha256Hex(rawToken), now, now, false};
        sessionIdByDigest_.emplace(stored.tokenDigest, sessionId);
        sessionIdsByPrincipal_[context.principalId].push_back(sessionId);
        sessionsById_.emplace(sessionId, std::move(stored));
        issued = IssuedSession{rawToken, std::move(context)};
    }
    // Same-device replacement revokes the previous session; notify outside
    // the lock.
    for (const auto id : replacedSessions) notifyRevocation(id, issued.context.principalId);
    return issued;
}

std::optional<AuthContext> SessionManager::authenticate(
    const std::string_view rawToken,
    const std::chrono::system_clock::time_point now)
{
    if (rawToken.size() < 43 || rawToken.size() > 128) return std::nullopt;
    const std::string digest = sha256Hex(rawToken);
    std::lock_guard lock(mutex_);
    const auto digestEntry = sessionIdByDigest_.find(digest);
    if (digestEntry == sessionIdByDigest_.end()) return std::nullopt;
    auto found = sessionsById_.find(digestEntry->second);
    if (found == sessionsById_.end() || found->second.revoked
        || found->second.context.expiresAt <= now) {
        if (found != sessionsById_.end()) {
            eraseSessionUnlocked(found->first);
        } else {
            sessionIdByDigest_.erase(digestEntry);
        }
        return std::nullopt;
    }
    if (found->second.context.tokenKind == TokenKind::Dashboard
        && now - found->second.lastSeenAt > std::chrono::minutes(30)) {
        const auto id = found->first;
        eraseSessionUnlocked(id);
        return std::nullopt;
    }
    found->second.lastSeenAt = now;
    return found->second.context;
}

bool SessionManager::revoke(const std::int64_t sessionId)
{
    std::string principalId;
    {
        std::lock_guard lock(mutex_);
        const auto found = sessionsById_.find(sessionId);
        if (found == sessionsById_.end()) return true;
        principalId = found->second.context.principalId;
        eraseSessionUnlocked(sessionId);
    }
    notifyRevocation(sessionId, principalId);
    return true;
}

bool SessionManager::revokeForPrincipal(
    const std::string_view principalId,
    const std::int64_t sessionId)
{
    std::string storedPrincipalId;
    bool revoked = false;
    {
        std::lock_guard lock(mutex_);
        const auto found = sessionsById_.find(sessionId);
        if (found == sessionsById_.end() || found->second.context.principalId != principalId) {
            return true;
        }
        storedPrincipalId = found->second.context.principalId;
        eraseSessionUnlocked(sessionId);
        revoked = true;
    }
    if (revoked) notifyRevocation(sessionId, storedPrincipalId);
    return true;
}

std::size_t SessionManager::revokePrincipal(const std::string_view principalId)
{
    std::vector<std::pair<std::int64_t, std::string>> revokedSessions;
    {
        std::lock_guard lock(mutex_);
        const auto principalSessions = sessionIdsByPrincipal_.find(std::string(principalId));
        if (principalSessions == sessionIdsByPrincipal_.end()) return 0;
        for (const auto id : principalSessions->second) {
            const auto found = sessionsById_.find(id);
            if (found == sessionsById_.end()) continue;
            revokedSessions.emplace_back(id, found->second.context.principalId);
            sessionIdByDigest_.erase(found->second.tokenDigest);
            sessionsById_.erase(found);
        }
        sessionIdsByPrincipal_.erase(principalSessions);
    }
    for (const auto &[id, principal] : revokedSessions) {
        notifyRevocation(id, principal);
    }
    return revokedSessions.size();
}

std::size_t SessionManager::revokeOtherSessions(
    const std::string_view principalId,
    const std::int64_t currentSessionId)
{
    std::vector<std::pair<std::int64_t, std::string>> revokedSessions;
    {
        std::lock_guard lock(mutex_);
        const auto principalSessions = sessionIdsByPrincipal_.find(std::string(principalId));
        if (principalSessions == sessionIdsByPrincipal_.end()) return 0;
        for (const auto id : principalSessions->second) {
            if (id == currentSessionId) continue;
            const auto found = sessionsById_.find(id);
            if (found == sessionsById_.end()) continue;
            revokedSessions.emplace_back(id, found->second.context.principalId);
            sessionIdByDigest_.erase(found->second.tokenDigest);
            sessionsById_.erase(found);
        }
        if (sessionsById_.find(currentSessionId) == sessionsById_.end()) {
            sessionIdsByPrincipal_.erase(principalSessions);
        } else {
            // The loop above only erased sessionsById_ entries; rebuild the
            // principal list so it holds exactly the surviving current session.
            auto &ids = principalSessions->second;
            ids.clear();
            ids.push_back(currentSessionId);
        }
    }
    for (const auto &[id, principal] : revokedSessions) {
        notifyRevocation(id, principal);
    }
    return revokedSessions.size();
}

std::vector<SessionView> SessionManager::activeSessions(
    const std::string_view principalId,
    const std::chrono::system_clock::time_point now) const
{
    std::lock_guard lock(mutex_);
    std::vector<SessionView> result;
    const auto principalSessions = sessionIdsByPrincipal_.find(std::string(principalId));
    if (principalSessions == sessionIdsByPrincipal_.end()) return result;
    for (const auto id : principalSessions->second) {
        const auto found = sessionsById_.find(id);
        if (found == sessionsById_.end()) continue;
        const auto &session = found->second;
        if (!session.revoked && session.context.expiresAt > now) {
            result.push_back(SessionView{
                id,
                session.context.deviceId,
                session.createdAt,
                session.lastSeenAt,
                session.context.expiresAt,
            });
        }
    }
    std::sort(result.begin(), result.end(), [](const auto &left, const auto &right) {
        return left.createdAt < right.createdAt;
    });
    return result;
}

void SessionManager::cleanup(const std::chrono::system_clock::time_point now)
{
    std::lock_guard lock(mutex_);
    cleanupUnlocked(now);
}

std::size_t SessionManager::size() const
{
    std::lock_guard lock(mutex_);
    return sessionsById_.size();
}

bool SessionManager::markReauthenticated(
    const std::int64_t sessionId,
    const std::chrono::system_clock::time_point now)
{
    std::lock_guard lock(mutex_);
    const auto found = sessionsById_.find(sessionId);
    if (found == sessionsById_.end() || found->second.revoked
        || found->second.context.expiresAt <= now) {
        return false;
    }
    found->second.context.reauthenticatedAt = now;
    return true;
}

bool SessionManager::hasRecentReauthentication(
    const AuthContext &context,
    const std::chrono::system_clock::time_point now,
    const std::chrono::minutes window) const
{
    return context.reauthenticatedAt.has_value()
        && now >= *context.reauthenticatedAt
        && now - *context.reauthenticatedAt <= window;
}

std::optional<std::string_view> SessionManager::parseBearer(
    const std::string_view authorization)
{
    constexpr std::string_view prefix = "Bearer ";
    if (!startsWith(authorization, prefix)) return std::nullopt;
    const std::string_view token = authorization.substr(prefix.size());
    if (token.size() < 43 || token.size() > 128
        || token.find_first_not_of(
               "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_")
            != std::string_view::npos) {
        return std::nullopt;
    }
    return token;
}

bool SessionManager::allowsPath(const TokenKind tokenKind, const std::string_view path)
{
    if (path == "/api/v1/events") return true;
    if (path == "/api/v1/system/health/ready") {
        return tokenKind == TokenKind::Administrator;
    }
    switch (tokenKind) {
    case TokenKind::User: return startsWith(path, "/api/v1/user/");
    case TokenKind::Administrator: return startsWith(path, "/api/v1/admin/");
    case TokenKind::Dashboard: return startsWith(path, "/api/v1/dashboard/");
    case TokenKind::MlTask: return startsWith(path, "/api/v1/internal/ml/");
    }
    return false;
}

bool SessionManager::hasRole(const AuthContext &context, const Role role)
{
    return std::find(context.roles.begin(), context.roles.end(), role) != context.roles.end();
}

std::size_t SessionManager::sessionLimit(const TokenKind tokenKind)
{
    switch (tokenKind) {
    case TokenKind::User: return 3;
    case TokenKind::Administrator: return 2;
    case TokenKind::Dashboard: return 2;
    case TokenKind::MlTask: return 1;
    }
    return 1;
}

void SessionManager::cleanupUnlocked(const std::chrono::system_clock::time_point now)
{
    for (auto iterator = sessionsById_.begin(); iterator != sessionsById_.end();) {
        if (iterator->second.revoked || iterator->second.context.expiresAt <= now) {
            sessionIdByDigest_.erase(iterator->second.tokenDigest);
            removeFromPrincipalIndexUnlocked(iterator->second.context.principalId, iterator->first);
            iterator = sessionsById_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

void SessionManager::eraseSessionUnlocked(const std::int64_t sessionId)
{
    const auto found = sessionsById_.find(sessionId);
    if (found == sessionsById_.end()) return;
    sessionIdByDigest_.erase(found->second.tokenDigest);
    removeFromPrincipalIndexUnlocked(found->second.context.principalId, sessionId);
    sessionsById_.erase(found);
}

void SessionManager::removeFromPrincipalIndexUnlocked(
    const std::string &principalId,
    const std::int64_t sessionId)
{
    const auto principalSessions = sessionIdsByPrincipal_.find(principalId);
    if (principalSessions == sessionIdsByPrincipal_.end()) return;
    auto &ids = principalSessions->second;
    ids.erase(std::remove(ids.begin(), ids.end(), sessionId), ids.end());
    if (ids.empty()) sessionIdsByPrincipal_.erase(principalSessions);
}

} // namespace ncs::core::application
