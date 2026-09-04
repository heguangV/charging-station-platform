#include "core/application/in_memory_user_account_repository.h"
#include "core/application/security_crypto.h"
#include "core/application/session_manager.h"
#include "core/application/user_identity_service.h"
#include "core/application/verification_code_service.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

using namespace ncs::core::application;

class TestRunner final {
public:
    void check(const bool condition, const std::string_view message)
    {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            ++failures_;
        }
    }

    int result() const { return failures_ == 0 ? 0 : 1; }

private:
    int failures_ = 0;
};

} // namespace

int main()
{
    TestRunner tests;
    PasswordHasher passwordHasher;
    const std::string encoded = passwordHasher.hash("correct-password");
    tests.check(encoded.rfind("pbkdf2-sha256$600000$", 0) == 0, "password hash is versioned");
    tests.check(encoded.find("correct-password") == std::string::npos, "hash omits plaintext");
    tests.check(passwordHasher.verify("correct-password", encoded), "correct password verifies");
    tests.check(!passwordHasher.verify("wrong-password", encoded), "wrong password is rejected");
    tests.check(!passwordHasher.verify("correct-password", "invalid"), "malformed hash is rejected");

    const std::string legacyEncoded = passwordHasher.hash("correct-password", 310000);
    tests.check(legacyEncoded.rfind("pbkdf2-sha256$310000$", 0) == 0,
        "legacy iteration count stays verifiable");
    tests.check(passwordHasher.verify("correct-password", legacyEncoded),
        "legacy hash verifies against current hasher");
    tests.check(!PasswordHasher::needsRehash(encoded), "current hash does not need re-hash");
    tests.check(PasswordHasher::needsRehash(legacyEncoded), "legacy hash is flagged for re-hash");
    tests.check(!PasswordHasher::needsRehash("invalid"), "malformed hash is never re-hashed");
    tests.check(!PasswordHasher::needsRehash("pbkdf2-sha256$abc$salt$digest"),
        "unparsable iteration count is never re-hashed");
    tests.check(!PasswordHasher::needsRehash("pbkdf2-sha256$99999$salt$digest"),
        "iteration count below the accepted range is never re-hashed");

    const auto now = std::chrono::system_clock::time_point(std::chrono::seconds(100000));
    SessionManager sessions;
    const auto first = sessions.issue(
        "user-1", "device-1", TokenKind::User, {Role::User}, now, std::chrono::hours(24));
    tests.check(first.has_value(), "first user session is issued");
    tests.check(first && first->accessToken.size() >= 43, "token has at least 256 random bits");
    tests.check(
        first && SessionManager::parseBearer("Bearer " + first->accessToken).has_value(),
        "strict bearer token is parsed");
    tests.check(!SessionManager::parseBearer("bearer invalid").has_value(), "invalid bearer is rejected");
    tests.check(
        first && sessions.authenticate(first->accessToken, now).has_value(),
        "stored digest authenticates raw token");
    tests.check(
        first && SessionManager::allowsPath(TokenKind::User, "/api/v1/user/me")
            && !SessionManager::allowsPath(TokenKind::User, "/api/v1/admin/users"),
        "user token is isolated from admin routes");

    const auto replacement = sessions.issue(
        "user-1", "device-1", TokenKind::User, {Role::User}, now, std::chrono::hours(24));
    tests.check(replacement.has_value(), "same terminal can replace its session");
    tests.check(
        first && !sessions.authenticate(first->accessToken, now).has_value(),
        "replaced token is revoked immediately");
    const auto second = sessions.issue(
        "user-1", "device-2", TokenKind::User, {Role::User}, now, std::chrono::hours(24));
    const auto third = sessions.issue(
        "user-1", "device-3", TokenKind::User, {Role::User}, now, std::chrono::hours(24));
    const auto fourth = sessions.issue(
        "user-1", "device-4", TokenKind::User, {Role::User}, now, std::chrono::hours(24));
    tests.check(second && third && !fourth, "user is limited to three terminals");
    tests.check(
        !sessions.issue(
            "user-2", "device-1", TokenKind::User, {Role::User}, now,
            std::chrono::hours(24 * 30) + std::chrono::seconds(1)),
        "user session cannot exceed thirty days");

    const auto admin1 = sessions.issue(
        "admin-1", "admin-a", TokenKind::Administrator, {Role::Operator},
        now, std::chrono::hours(8));
    const auto admin2 = sessions.issue(
        "admin-1", "admin-b", TokenKind::Administrator, {Role::Owner},
        now, std::chrono::hours(8));
    const auto admin3 = sessions.issue(
        "admin-1", "admin-c", TokenKind::Administrator, {Role::Owner},
        now, std::chrono::hours(8));
    tests.check(admin1 && admin2 && !admin3, "administrator is limited to two terminals");
    tests.check(
        !sessions.issue(
            "viewer-1", "browser", TokenKind::Dashboard, {Role::Viewer}, now,
            std::chrono::hours(8) + std::chrono::seconds(1)),
        "dashboard session cannot exceed eight hours");
    tests.check(
        admin1 && !sessions.hasRecentReauthentication(admin1->context, now),
        "sensitive operation initially requires reauthentication");
    tests.check(
        admin1 && sessions.markReauthenticated(admin1->context.sessionId, now),
        "reauthentication timestamp is recorded");
    const auto refreshedAdmin = admin1 ? sessions.authenticate(admin1->accessToken, now) : std::nullopt;
    tests.check(
        refreshedAdmin && sessions.hasRecentReauthentication(*refreshedAdmin, now + std::chrono::minutes(14)),
        "reauthentication is valid within fifteen minutes");
    tests.check(
        refreshedAdmin && !sessions.hasRecentReauthentication(*refreshedAdmin, now + std::chrono::minutes(16)),
        "reauthentication expires after fifteen minutes");
    tests.check(
        admin1 && sessions.revoke(admin1->context.sessionId)
            && !sessions.authenticate(admin1->accessToken, now).has_value(),
        "revocation invalidates token immediately");

    // Revocation observer: fires on every revoke path, never while the
    // SessionManager lock is held (proved by a re-entrant size() call, which
    // would deadlock on the non-recursive mutex if the observer ran under it).
    SessionManager observedSessions;
    std::vector<std::pair<std::int64_t, std::string>> notifications;
    observedSessions.setRevocationObserver(
        [&observedSessions, &notifications](
            const std::int64_t sessionId,
            const std::string_view principalId) {
            notifications.emplace_back(sessionId, std::string(principalId));
            observedSessions.size();
        });
    const auto observedA = observedSessions.issue(
        "user-a", "terminal-a", TokenKind::User, {Role::User}, now, std::chrono::hours(24));
    const auto observedB = observedSessions.issue(
        "user-a", "terminal-b", TokenKind::User, {Role::User}, now, std::chrono::hours(24));
    tests.check(observedA && observedB, "observed sessions are issued");
    const auto observedReplacement = observedSessions.issue(
        "user-a", "terminal-a", TokenKind::User, {Role::User}, now + std::chrono::seconds(1),
        std::chrono::hours(24));
    tests.check(
        observedA && observedReplacement
            && notifications.size() == 1
            && notifications.front().first == observedA->context.sessionId
            && notifications.front().second == "user-a",
        "same-device replacement notifies the revoked session");
    notifications.clear();

    const auto observedC = observedSessions.issue(
        "user-a", "terminal-c", TokenKind::User, {Role::User}, now + std::chrono::seconds(2),
        std::chrono::hours(24));
    tests.check(
        observedC && observedSessions.revoke(observedC->context.sessionId)
            && notifications.size() == 1
            && notifications.front().first == observedC->context.sessionId,
        "revoke() notifies exactly the revoked session");
    notifications.clear();

    const auto observedD = observedSessions.issue(
        "user-b", "terminal-d", TokenKind::User, {Role::User}, now + std::chrono::seconds(3),
        std::chrono::hours(24));
    tests.check(
        observedD
            && observedSessions.revokeForPrincipal("user-b", observedD->context.sessionId)
            && notifications.size() == 1
            && notifications.front().second == "user-b",
        "revokeForPrincipal() notifies with the stored principal");
    notifications.clear();

    tests.check(
        observedSessions.revokePrincipal("user-a") == 2
            && notifications.size() == 2,
        "revokePrincipal() notifies each revoked session");
    const auto revokedIds = [&notifications] {
        std::vector<std::int64_t> ids;
        for (const auto &[id, principal] : notifications) ids.push_back(id);
        std::sort(ids.begin(), ids.end());
        return ids;
    }();
    const auto expectedIds = [&observedReplacement, &observedB] {
        std::vector<std::int64_t> ids{observedReplacement->context.sessionId,
            observedB->context.sessionId};
        std::sort(ids.begin(), ids.end());
        return ids;
    }();
    tests.check(revokedIds == expectedIds, "revokePrincipal() notifications carry the right ids");
    notifications.clear();

    const auto kept = observedSessions.issue(
        "user-c", "terminal-e", TokenKind::User, {Role::User}, now + std::chrono::seconds(4),
        std::chrono::hours(24));
    const auto other1 = observedSessions.issue(
        "user-c", "terminal-f", TokenKind::User, {Role::User}, now + std::chrono::seconds(5),
        std::chrono::hours(24));
    const auto other2 = observedSessions.issue(
        "user-c", "terminal-g", TokenKind::User, {Role::User}, now + std::chrono::seconds(6),
        std::chrono::hours(24));
    tests.check(
        kept && other1 && other2
            && observedSessions.revokeOtherSessions("user-c", kept->context.sessionId) == 2
            && notifications.size() == 2
            && kept && observedSessions.authenticate(kept->accessToken, now).has_value(),
        "revokeOtherSessions() notifies revoked peers and keeps the current session");

    VerificationCodeService developmentCodes(true);
    const auto issued = developmentCodes.issue("13800138000", "LOGIN", now);
    tests.check(
        issued.status == CodeIssueStatus::Issued && issued.developmentCode.has_value(),
        "development issue exposes simulated code");
    const auto cooldown = developmentCodes.issue("13800138000", "LOGIN", now + std::chrono::seconds(1));
    tests.check(
        cooldown.status == CodeIssueStatus::Cooldown && cooldown.retryAfterSec == 59,
        "verification code has sixty-second cooldown");
    tests.check(
        issued.developmentCode
            && developmentCodes.verify(
                "13800138000", "LOGIN", *issued.developmentCode, now + std::chrono::minutes(1))
                == CodeVerifyStatus::Valid,
        "verification code is valid once within ten minutes");
    tests.check(
        issued.developmentCode
            && developmentCodes.verify(
                "13800138000", "LOGIN", *issued.developmentCode, now + std::chrono::minutes(1))
                == CodeVerifyStatus::NotFound,
        "verification code cannot be reused");

    const auto expiring = developmentCodes.issue(
        "13800138001", "REGISTER", now);
    tests.check(
        expiring.developmentCode
            && developmentCodes.verify(
                "13800138001", "REGISTER", *expiring.developmentCode,
                now + std::chrono::minutes(10)) == CodeVerifyStatus::Expired,
        "verification code expires after ten minutes");
    const auto locking = developmentCodes.issue("13800138002", "RESET_PASSWORD", now);
    CodeVerifyStatus lastStatus = CodeVerifyStatus::Invalid;
    for (int attempt = 0; attempt < 5; ++attempt) {
        lastStatus = developmentCodes.verify(
            "13800138002", "RESET_PASSWORD", "999999", now + std::chrono::seconds(2));
    }
    tests.check(lastStatus == CodeVerifyStatus::Locked, "five failures lock verification code");

    VerificationCodeService productionCodes(false);
    const auto productionIssue = productionCodes.issue("13800138003", "LOGIN", now);
    tests.check(
        productionIssue.status == CodeIssueStatus::Issued
            && !productionIssue.developmentCode.has_value(),
        "non-development issue never returns code");

    SessionManager indexed;
    for (int principal = 0; principal < 100; ++principal) {
        const auto name = "p" + std::to_string(principal);
        for (int device = 0; device < 3; ++device) {
            tests.check(
                indexed.issue(
                    name, "d" + std::to_string(device), TokenKind::User, {Role::User}, now,
                    std::chrono::hours(1))
                    .has_value(),
                "principal index issues up to the terminal limit");
        }
        tests.check(
            !indexed.issue(name, "d3", TokenKind::User, {Role::User}, now, std::chrono::hours(1)),
            "principal limit is enforced without scanning the whole table");
    }
    tests.check(indexed.size() == 300, "independent principals accumulate sessions");
    const std::string indexedToken = [&] {
        const auto session = indexed.issue(
            "p0", "d0", TokenKind::User, {Role::User}, now, std::chrono::hours(1));
        tests.check(session.has_value(), "same-terminal replacement works through the index");
        if (session) {
            indexed.revokeOtherSessions("p0", session->context.sessionId);
            return session->accessToken;
        }
        return std::string();
    }();
    tests.check(
        indexed.authenticate(indexedToken, now).has_value()
            && indexed.activeSessions("p0", now).size() == 1,
        "other-session revocation keeps only the current terminal");
    tests.check(indexed.revokePrincipal("p7") == 3, "principal revocation removes own sessions");
    tests.check(indexed.size() == 295, "principal revocation keeps other principals");
    tests.check(
        indexed.authenticate(indexedToken, now).has_value(),
        "principal revocation leaves unrelated tokens valid");
    indexed.cleanup(now + std::chrono::hours(2));
    tests.check(indexed.size() == 0, "cleanup removes every expired session");
    tests.check(
        !indexed.authenticate(indexedToken, now + std::chrono::hours(2)).has_value(),
        "expired token cannot authenticate after cleanup");

    InMemoryUserAccountRepository accounts;
    VerificationCodeService codes(true);
    UserIdentityService identity(accounts, sessions, codes);
    UserAccount legacyAccount;
    legacyAccount.username = "legacy_user";
    legacyAccount.phone = "13800139000";
    legacyAccount.passwordHash = passwordHasher.hash("legacy-password", 310000);
    tests.check(
        accounts.create(legacyAccount) == AccountWriteResult::Success,
        "legacy account is created for re-hash test");
    const auto wrongPassword = identity.loginPassword(
        "legacy_user", "not-the-password", "device-1", now);
    tests.check(!wrongPassword.ok(), "wrong password on legacy account fails");
    const auto legacyStored = accounts.findByLoginName("legacy_user");
    tests.check(
        legacyStored && legacyStored->passwordHash
            && legacyStored->passwordHash->rfind("pbkdf2-sha256$310000$", 0) == 0,
        "failed login does not re-hash the stored digest");
    const auto legacyLogin = identity.loginPassword(
        "legacy_user", "legacy-password", "device-1", now);
    tests.check(legacyLogin.ok(), "legacy account still logs in with its password");
    const auto rehashed = accounts.findByLoginName("legacy_user");
    tests.check(
        rehashed && rehashed->passwordHash
            && rehashed->passwordHash->rfind("pbkdf2-sha256$600000$", 0) == 0,
        "successful login transparently re-hashes a legacy digest");
    const auto relaunch = identity.loginPassword(
        "legacy_user", "legacy-password", "device-2", now + std::chrono::seconds(1));
    tests.check(relaunch.ok(), "re-hashed account keeps accepting its password");
    return tests.result();
}
