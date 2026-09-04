#include "core/application/session_manager.h"
#include "server/websocket/websocket_routes.h"

#include <chrono>
#include <iostream>
#include <string>
#include <string_view>

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

crow::request requestWithAuthorization(std::string authorization)
{
    crow::request request;
    request.url = "/api/v1/events";
    request.raw_url = request.url;
    request.remote_ip_address = "127.0.0.1";
    request.add_header("Authorization", std::move(authorization));
    return request;
}

} // namespace

int main()
{
    using ncs::server::websocket::HandshakeDecision;

    TestRunner tests;
    const auto now = std::chrono::system_clock::time_point(std::chrono::seconds(1788500000));
    SessionManager sessions;
    const auto user = sessions.issue(
        "user:1", "device-1", TokenKind::User, {Role::User}, now, std::chrono::hours(24));
    const auto admin = sessions.issue(
        "admin:1", "admin-a", TokenKind::Administrator, {Role::Operator}, now,
        std::chrono::hours(8));
    const auto dashboard = sessions.issue(
        "dashboard:1", "browser", TokenKind::Dashboard, {Role::Viewer}, now,
        std::chrono::hours(8));
    const auto ml = sessions.issue(
        "ml:1", "worker", TokenKind::MlTask, {Role::MlWorker}, now,
        std::chrono::hours(1));
    tests.check(user && admin && dashboard && ml, "sessions of all token kinds are issued");

    const auto rejected = ncs::server::websocket::authorizeHandshake(
        requestWithAuthorization(""), sessions, now);
    tests.check(
        rejected.decision == HandshakeDecision::Unauthorized,
        "a missing authorization header is rejected");

    const auto garbage = ncs::server::websocket::authorizeHandshake(
        requestWithAuthorization("Bearer not-a-real-token-00000000000000000000000000"),
        sessions, now);
    tests.check(
        garbage.decision == HandshakeDecision::Unauthorized,
        "an unknown token is rejected");

    const auto acceptedUser = ncs::server::websocket::authorizeHandshake(
        requestWithAuthorization("Bearer " + user->accessToken), sessions, now);
    tests.check(
        acceptedUser.decision == HandshakeDecision::Accepted
            && acceptedUser.auth
            && acceptedUser.auth->tokenKind == TokenKind::User
            && acceptedUser.auth->sessionId == user->context.sessionId,
        "a valid user token is accepted with its context");

    const auto acceptedAdmin = ncs::server::websocket::authorizeHandshake(
        requestWithAuthorization("Bearer " + admin->accessToken), sessions, now);
    tests.check(
        acceptedAdmin.decision == HandshakeDecision::Accepted
            && acceptedAdmin.auth
            && acceptedAdmin.auth->tokenKind == TokenKind::Administrator,
        "a valid administrator token is accepted");

    const auto acceptedDashboard = ncs::server::websocket::authorizeHandshake(
        requestWithAuthorization("Bearer " + dashboard->accessToken), sessions, now);
    tests.check(
        acceptedDashboard.decision == HandshakeDecision::Accepted
            && acceptedDashboard.auth
            && acceptedDashboard.auth->tokenKind == TokenKind::Dashboard,
        "a valid dashboard token is accepted");

    const auto forbiddenMl = ncs::server::websocket::authorizeHandshake(
        requestWithAuthorization("Bearer " + ml->accessToken), sessions, now);
    tests.check(
        forbiddenMl.decision == HandshakeDecision::Forbidden,
        "an ML task token is rejected with Forbidden");

    tests.check(sessions.revoke(ml->context.sessionId), "the ML session is revoked");
    const auto revoked = ncs::server::websocket::authorizeHandshake(
        requestWithAuthorization("Bearer " + ml->accessToken), sessions, now);
    tests.check(
        revoked.decision == HandshakeDecision::Unauthorized,
        "a revoked token is rejected");

    const auto expiring = sessions.issue(
        "user:2", "device-2", TokenKind::User, {Role::User}, now,
        std::chrono::hours(1));
    const auto expired = ncs::server::websocket::authorizeHandshake(
        requestWithAuthorization("Bearer " + expiring->accessToken), sessions,
        now + std::chrono::hours(2));
    tests.check(
        expired.decision == HandshakeDecision::Unauthorized,
        "an expired token is rejected");

    return tests.result();
}
