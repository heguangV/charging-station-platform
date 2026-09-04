#include "core/application/readiness_probe.h"
#include "core/application/session_manager.h"
#include "core/domain/error_code.h"
#include "infrastructure/files/structured_logger.h"
#include "server/controller/api_response.h"
#include "server/controller/api_routes.h"
#include "server/controller/health_routes.h"
#include "server/controller/request_validation.h"
#include "server/middleware/authorization.h"
#include "server/middleware/request_policy_middleware.h"
#include "server/server_app.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <chrono>
#include <iostream>
#include <string_view>
#include <unordered_set>

namespace {

using namespace ncs;

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

class ReadyProbe final : public core::application::ReadinessProbe {
public:
    core::application::ReadinessStatus check() override
    {
        return {true, true, true, true};
    }
};

QJsonObject bodyObject(const crow::response &response)
{
    return QJsonDocument::fromJson(QByteArray::fromStdString(response.body)).object();
}

crow::request requestFor(std::string url)
{
    crow::request request;
    request.url = std::move(url);
    request.raw_url = request.url;
    request.remote_ip_address = "127.0.0.1";
    return request;
}

std::string utf8Path(const QString &path)
{
    const QByteArray bytes = path.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

} // namespace

int main()
{
    TestRunner tests;
    using core::domain::ErrorCode;
    tests.check(core::domain::httpStatus(ErrorCode::InvalidArgument) == 400, "invalid argument maps 400");
    tests.check(core::domain::httpStatus(ErrorCode::ValidationFailed) == 422, "validation maps 422");
    tests.check(core::domain::httpStatus(ErrorCode::IdempotencyConflict) == 409, "idempotency maps 409");
    tests.check(core::domain::httpStatus(ErrorCode::RateLimited) == 429, "rate limit maps 429");
    tests.check(core::domain::httpStatus(ErrorCode::DatabaseError) == 503, "database maps 503");
    tests.check(core::domain::httpStatus(ErrorCode::Unauthorized) == 401, "unauthorized maps 401");
    tests.check(core::domain::httpStatus(ErrorCode::Forbidden) == 403, "forbidden maps 403");

    infrastructure::files::RequestLogScope responseScope(
        "2cb640c6-6995-4be5-9161-f0e2c1201a53");
    const auto success = server::controller::successResponse(
        QJsonObject{{QStringLiteral("value"), 1}}, 201);
    const auto successBody = bodyObject(success);
    tests.check(success.code == 201 && successBody.size() == 6, "success uses six-field envelope");
    tests.check(successBody.value(QStringLiteral("success")).toBool(), "success flag is true");
    tests.check(successBody.value(QStringLiteral("data")).isObject(), "success data is object");
    const auto failure = server::controller::errorResponse(
        ErrorCode::InternalError,
        "SELECT token FROM /srv/private/database.sqlite",
        "retry");
    const auto failureBody = bodyObject(failure);
    tests.check(failure.code == 500 && failureBody.size() == 6, "failure uses six-field envelope");
    tests.check(
        !failure.body.empty() && failure.body.find("SELECT") == std::string::npos
            && failure.body.find("/srv/") == std::string::npos,
        "failure diagnostic is sanitized");

    crow::request jsonRequest = requestFor("/test");
    jsonRequest.add_header("Content-Type", "application/json; charset=utf-8");
    jsonRequest.body = R"({"name":"valid","version":3})";
    auto parsed = server::controller::parseJsonObject(
        jsonRequest, {"name", "version"}, {"name", "version"});
    tests.check(parsed.object.has_value(), "allowed JSON object is accepted");
    tests.check(
        parsed.object && server::controller::validStringField(*parsed.object, "name", 3, 32),
        "string field boundary is enforced");
    tests.check(
        parsed.object && server::controller::validIntegerField(*parsed.object, "version", 1, 100),
        "integer field boundary is enforced");
    jsonRequest.body = R"({"name":"valid","unknown":1})";
    tests.check(
        !server::controller::parseJsonObject(jsonRequest, {"name"}, {"name"}).object,
        "unknown JSON field is rejected");
    jsonRequest.body = R"({"name":)";
    tests.check(
        !server::controller::parseJsonObject(jsonRequest, {"name"}).object,
        "malformed JSON is rejected");

    crow::request pageRequest = requestFor("/items");
    pageRequest.raw_url = "/items?page=2&pageSize=100&sort=-createdAt&status=1";
    pageRequest.url_params = crow::query_string(pageRequest.raw_url);
    const auto pagination = server::controller::parsePagination(
        pageRequest, {"createdAt", "-createdAt"}, {"status"});
    tests.check(
        pagination && pagination->page == 2 && pagination->pageSize == 100
            && pagination->sort == "-createdAt" && pagination->filters.at("status") == "1",
        "pagination, filter and sort whitelists are accepted");
    crow::request badPage = requestFor("/items");
    badPage.raw_url = "/items?page=0&pageSize=101&sort=sql";
    badPage.url_params = crow::query_string(badPage.raw_url);
    tests.check(
        !server::controller::parsePagination(badPage, {"createdAt"}),
        "pagination bounds and unknown sort are rejected");
    crow::request unknownFilter = requestFor("/items");
    unknownFilter.raw_url = "/items?sql=drop";
    unknownFilter.url_params = crow::query_string(unknownFilter.raw_url);
    tests.check(
        !server::controller::parsePagination(unknownFilter, {}, {"status"}),
        "unknown filter parameter is rejected");
    pageRequest.add_header("Idempotency-Key", "2cb640c6-6995-4be5-9161-f0e2c1201a53");
    tests.check(
        server::controller::idempotencyKey(pageRequest).has_value(),
        "canonical Idempotency-Key header is accepted");

    QTemporaryDir logs;
    infrastructure::files::StructuredLogger logger({utf8Path(logs.path()),
        infrastructure::files::LogLevel::Info, 30, false});
    server::middleware::RequestPolicyMiddleware policy;
    policy.configure(logger, {"https://dashboard.local"});
    server::middleware::RequestPolicyMiddleware::context policyContext;
    crow::request policyRequest = requestFor("/api/v1/user/me");
    crow::response policyResponse;
    policy.before_handle(policyRequest, policyResponse, policyContext);
    tests.check(
        server::middleware::RequestPolicyMiddleware::validRequestId(policyContext.requestId),
        "missing request ID is generated as UUID");
    policy.after_handle(policyRequest, policyResponse, policyContext);
    tests.check(
        policyResponse.get_header_value("X-Request-ID") == policyContext.requestId,
        "request ID is returned in response");
    tests.check(
        policyResponse.get_header_value("Strict-Transport-Security") == "max-age=31536000",
        "TLS response includes HSTS");

    server::middleware::RequestPolicyMiddleware::context tokenContext;
    crow::request tokenRequest = requestFor("/api/v1/user/me");
    tokenRequest.raw_url = "/api/v1/user/me?accessToken=secret";
    crow::response tokenResponse;
    policy.before_handle(tokenRequest, tokenResponse, tokenContext);
    tests.check(tokenResponse.code == 400, "URL credential is rejected");
    policy.after_handle(tokenRequest, tokenResponse, tokenContext);

    server::middleware::RequestPolicyMiddleware::context corsContext;
    crow::request corsRequest = requestFor("/api/v1/user/me");
    corsRequest.add_header("Origin", "https://evil.invalid");
    crow::response corsResponse;
    policy.before_handle(corsRequest, corsResponse, corsContext);
    tests.check(corsResponse.code == 403, "unlisted CORS origin is rejected");
    policy.after_handle(corsRequest, corsResponse, corsContext);

    server::middleware::RequestPolicyMiddleware::context preflightContext;
    crow::request preflight = requestFor("/api/v1/dashboard/summary");
    preflight.method = crow::HTTPMethod::Options;
    preflight.add_header("Origin", "https://dashboard.local");
    crow::response preflightResponse;
    policy.before_handle(preflight, preflightResponse, preflightContext);
    policy.after_handle(preflight, preflightResponse, preflightContext);
    tests.check(
        preflightResponse.code == 204
            && preflightResponse.get_header_value("Access-Control-Allow-Origin")
                == "https://dashboard.local"
            && preflightResponse.get_header_value("Access-Control-Allow-Headers")
                .find("Authorization") != std::string::npos,
        "allowlisted CORS preflight returns only approved methods and headers");

    tests.check(
        server::middleware::RequestPolicyMiddleware::bodyLimitForPath(
            "/api/v1/internal/ml/predictions/batch") == 8 * 1024 * 1024,
        "prediction batch limit is eight MiB");
    tests.check(
        server::middleware::RequestPolicyMiddleware::bodyLimitForPath(
            "/api/v1/user/me/avatar") == 5 * 1024 * 1024,
        "avatar limit is five MiB");
    tests.check(
        server::middleware::RequestPolicyMiddleware::deadlineForPath(
            "/api/v1/admin/stats/revenue") == std::chrono::seconds(30),
        "statistics deadline is thirty seconds");
    server::middleware::RequestPolicyMiddleware::context timeoutContext;
    crow::request timeoutRequest = requestFor("/api/v1/user/me");
    crow::response timeoutResponse;
    policy.before_handle(timeoutRequest, timeoutResponse, timeoutContext);
    timeoutContext.startedAt -= std::chrono::seconds(11);
    policy.after_handle(timeoutRequest, timeoutResponse, timeoutContext);
    tests.check(
        timeoutResponse.code == 503
            && bodyObject(timeoutResponse).value(QStringLiteral("code")).toInt() == 12,
        "expired ordinary request returns safe timeout response");

    server::middleware::RateLimiter limiter;
    limiter.configure(1.0, 1.0);
    const auto tick = std::chrono::steady_clock::now();
    tests.check(limiter.allow("client", tick).allowed, "first rate-limited request is allowed");
    tests.check(!limiter.allow("client", tick).allowed, "burst overflow is rejected");
    tests.check(limiter.allow("client", tick + std::chrono::seconds(1)).allowed, "rate token refills");

    core::application::SessionManager sessions;
    core::application::UnavailableReadinessProbe unavailable;
    server::ServerApp healthApp;
    server::controller::ApiRoutes healthApi(healthApp);
    server::controller::HealthRoutes healthRoutes(healthApi, unavailable, sessions);
    healthApp.validate();
    crow::request liveRequest = requestFor("/api/v1/system/health/live");
    crow::response liveResponse;
    healthApp.handle_full(liveRequest, liveResponse);
    tests.check(
        liveResponse.code == 200
            && bodyObject(liveResponse).value(QStringLiteral("status")) == QStringLiteral("UP"),
        "live route reports process UP without dependency checks");
    crow::request readyRequest = requestFor("/api/v1/system/health/ready");
    crow::response readyResponse;
    healthApp.handle_full(readyRequest, readyResponse);
    const QJsonObject readyBody = bodyObject(readyResponse);
    tests.check(
        readyResponse.code == 503
            && readyBody.value(QStringLiteral("status")) == QStringLiteral("DOWN"),
        "ready route reports DOWN while database adapter is unavailable");
    tests.check(
        readyResponse.body.find("sqlite") == std::string::npos
            && readyResponse.body.find("path") == std::string::npos,
        "ready route exposes only safe boolean checks");

    ReadyProbe readyProbe;
    server::ServerApp readyApp;
    server::controller::ApiRoutes readyApi(readyApp);
    server::controller::HealthRoutes readyRoutes(readyApi, readyProbe, sessions);
    readyApp.validate();
    crow::response availableResponse;
    readyApp.handle_full(readyRequest, availableResponse);
    tests.check(availableResponse.code == 200, "ready route reports UP when all checks pass");
    crow::request remoteReady = requestFor("/api/v1/system/health/ready");
    remoteReady.remote_ip_address = "10.0.0.8";
    crow::response remoteDenied;
    readyApp.handle_full(remoteReady, remoteDenied);
    tests.check(remoteDenied.code == 401, "remote ready check requires administrator token");
    const auto now = std::chrono::system_clock::now();
    const auto admin = sessions.issue(
        "admin-health", "ops-terminal", core::application::TokenKind::Administrator,
        {core::application::Role::Operator}, now, std::chrono::hours(1));
    tests.check(admin.has_value(), "health-check administrator session is issued");
    if (admin) remoteReady.add_header("Authorization", "Bearer " + admin->accessToken);
    crow::response remoteAllowed;
    readyApp.handle_full(remoteReady, remoteAllowed);
    tests.check(remoteAllowed.code == 200, "authorized administrator can read remote readiness");
    return tests.result();
}
