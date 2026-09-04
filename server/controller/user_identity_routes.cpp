#include "server/controller/user_identity_routes.h"

#include "server/controller/api_response.h"
#include "server/controller/async_response.h"
#include "server/controller/request_validation.h"
#include "server/controller/user_identity_dto.h"
#include "server/middleware/authorization.h"

#include <QJsonArray>
#include <QJsonObject>

#include <asio/ip/address.hpp>

#include <charconv>

namespace ncs::server::controller {
namespace {

using core::application::AuthContext;
using core::application::ServiceResult;
using core::application::UserAccount;
using core::domain::ErrorCode;

std::string text(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toString().toStdString();
}

std::optional<std::string> optionalText(const QJsonObject &object, const char *key)
{
    const QJsonValue value = object.value(QLatin1String(key));
    return value.isString() ? std::optional<std::string>(value.toString().toStdString())
                            : std::nullopt;
}

std::optional<std::int64_t> positiveInteger(const std::string_view value)
{
    std::int64_t result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() || result <= 0) {
        return std::nullopt;
    }
    return result;
}

crow::response serviceError(const ErrorCode code)
{
    switch (code) {
    case ErrorCode::AlreadyExists:
        return errorResponse(code, "account already exists", "用户名或手机号已被使用");
    case ErrorCode::UserFrozen:
        return errorResponse(code, "user is frozen", "账号已被冻结，请联系客服");
    case ErrorCode::CodeInvalid:
        return errorResponse(code, "verification code is invalid", "验证码错误或次数已用尽");
    case ErrorCode::CodeExpired:
        return errorResponse(code, "verification code expired", "验证码已过期，请重新获取");
    case ErrorCode::VersionConflict:
        return errorResponse(code, "resource version conflict", "资料已更新，请刷新后重试");
    case ErrorCode::ActiveFlowExists:
        return errorResponse(code, "active flow exists", "存在进行中的充电流程，暂时不能注销");
    case ErrorCode::NotFound:
        return errorResponse(code, "resource not found", "未找到相关数据");
    case ErrorCode::Unauthorized:
        return errorResponse(code, "authentication failed", "账号或凭据错误");
    default:
        return errorResponse(code, "request validation failed", "请求内容不符合要求");
    }
}

std::optional<AuthContext> userAuth(
    const crow::request &request,
    core::application::SessionManager &sessions,
    crow::response &failure)
{
    const auto result = middleware::authorize(
        request, sessions,
        {core::application::TokenKind::User},
        {core::application::Role::User},
        std::chrono::system_clock::now());
    if (!result.context) failure = serviceError(result.error);
    return result.context;
}

bool loopback(const std::string_view addressText)
{
    asio::error_code error;
    const auto address = asio::ip::make_address(addressText, error);
    return !error && address.is_loopback();
}

crow::response accountResult(const ServiceResult<UserAccount> &result)
{
    return result.ok() ? successResponse(userJson(*result.value, true))
                       : serviceError(result.error);
}

void finish(crow::response &response, crow::response result)
{
    response = std::move(result);
    response.end();
}

} // namespace

UserIdentityRoutes::UserIdentityRoutes(
    ApiRoutes &routes,
    core::application::UserIdentityService &identity,
    core::application::SessionManager &sessions,
    core::application::BoundedExecutor &blockingExecutor,
    const bool developmentMode)
{
    routes.route("/user/auth/sms/code").methods(crow::HTTPMethod::POST)
        ([&identity, developmentMode](const crow::request &request) {
            const auto parsed = parseJsonObject(request, {"phone", "purpose"}, {"phone", "purpose"});
            if (!parsed.object) return serviceError(ErrorCode::InvalidArgument);
            const auto result = identity.issueCode(
                text(*parsed.object, "phone"), text(*parsed.object, "purpose"),
                std::chrono::system_clock::now());
            if (!result.value) return serviceError(result.error);
            QJsonObject data{
                {QStringLiteral("expiresAt"), QJsonValue(static_cast<qint64>(
                     unixSeconds(result.value->expiresAt)))},
                {QStringLiteral("retryAfterSec"), result.value->retryAfterSec > 0
                     ? result.value->retryAfterSec : 60},
                {QStringLiteral("maxAttempts"), 5},
            };
            if (developmentMode && loopback(request.remote_ip_address)
                && result.value->developmentCode) {
                data.insert(
                    QStringLiteral("developmentCode"),
                    QString::fromStdString(*result.value->developmentCode));
            }
            if (result.error == ErrorCode::RateLimited) {
                auto response = errorResponse(
                    ErrorCode::RateLimited, "verification code cooldown",
                    "请稍后再获取验证码",
                    QJsonObject{{QStringLiteral("retryAfterSec"), result.value->retryAfterSec}});
                response.set_header("Retry-After", std::to_string(result.value->retryAfterSec));
                return response;
            }
            return successResponse(std::move(data));
        });

    routes.route("/user/auth/register").methods(crow::HTTPMethod::POST)
        ([&identity, &blockingExecutor](
             const crow::request &request, crow::response &response) {
            const auto parsed = parseJsonObject(
                request,
                {"username", "phone", "password", "smsCode", "deviceId"},
                {"username", "phone", "password", "smsCode", "deviceId"});
            if (!parsed.object) {
                finish(response, serviceError(ErrorCode::InvalidArgument));
                return;
            }
            auto username = text(*parsed.object, "username");
            auto phone = text(*parsed.object, "phone");
            auto password = text(*parsed.object, "password");
            auto smsCode = text(*parsed.object, "smsCode");
            auto deviceId = text(*parsed.object, "deviceId");
            dispatchBlocking(request, response, blockingExecutor,
                [&identity, username = std::move(username), phone = std::move(phone),
                    password = std::move(password), smsCode = std::move(smsCode),
                    deviceId = std::move(deviceId)]() mutable {
                    const auto result = identity.registerUser(
                        std::move(username), std::move(phone), std::move(password),
                        std::move(smsCode), std::move(deviceId),
                        std::chrono::system_clock::now());
                    return result.ok() ? successResponse(loginJson(*result.value), 201)
                                       : serviceError(result.error);
                });
        });

    routes.route("/user/auth/login/password").methods(crow::HTTPMethod::POST)
        ([&identity, &blockingExecutor](
             const crow::request &request, crow::response &response) {
            const auto parsed = parseJsonObject(
                request, {"loginName", "password", "deviceId"},
                {"loginName", "password", "deviceId"});
            if (!parsed.object) {
                finish(response, serviceError(ErrorCode::InvalidArgument));
                return;
            }
            auto loginName = text(*parsed.object, "loginName");
            auto password = text(*parsed.object, "password");
            auto deviceId = text(*parsed.object, "deviceId");
            dispatchBlocking(request, response, blockingExecutor,
                [&identity, loginName = std::move(loginName),
                    password = std::move(password), deviceId = std::move(deviceId)]() mutable {
                    const auto result = identity.loginPassword(
                        std::move(loginName), std::move(password), std::move(deviceId),
                        std::chrono::system_clock::now());
                    return result.ok() ? successResponse(loginJson(*result.value))
                                       : serviceError(result.error);
                });
        });

    routes.route("/user/auth/login/sms").methods(crow::HTTPMethod::POST)
        ([&identity](const crow::request &request) {
            const auto parsed = parseJsonObject(
                request, {"phone", "smsCode", "deviceId"},
                {"phone", "smsCode", "deviceId"});
            if (!parsed.object) return serviceError(ErrorCode::InvalidArgument);
            const auto result = identity.loginSms(
                text(*parsed.object, "phone"), text(*parsed.object, "smsCode"),
                text(*parsed.object, "deviceId"), std::chrono::system_clock::now());
            return result.ok() ? successResponse(loginJson(*result.value))
                               : serviceError(result.error);
        });

    routes.route("/user/auth/logout").methods(crow::HTTPMethod::POST)
        ([&sessions](const crow::request &request) {
            const auto bearer = core::application::SessionManager::parseBearer(
                request.get_header_value("Authorization"));
            if (!bearer) return serviceError(ErrorCode::Unauthorized);
            const auto auth = sessions.authenticate(*bearer, std::chrono::system_clock::now());
            if (auth && auth->tokenKind == core::application::TokenKind::User) {
                sessions.revoke(auth->sessionId);
            }
            return successResponse();
        });

    routes.route("/user/sessions").methods(crow::HTTPMethod::GET)
        ([&sessions](const crow::request &request) {
            crow::response failure;
            const auto auth = userAuth(request, sessions, failure);
            if (!auth) return failure;
            QJsonArray items;
            for (const auto &session : sessions.activeSessions(
                     auth->principalId, std::chrono::system_clock::now())) {
                items.append(QJsonObject{
                    {QStringLiteral("sessionId"), QJsonValue(static_cast<qint64>(
                         session.sessionId))},
                    {QStringLiteral("deviceId"), QString::fromStdString(session.deviceId)},
                    {QStringLiteral("createdAt"), QJsonValue(static_cast<qint64>(
                         unixSeconds(session.createdAt)))},
                    {QStringLiteral("lastSeenAt"), QJsonValue(static_cast<qint64>(
                         unixSeconds(session.lastSeenAt)))},
                    {QStringLiteral("expiresAt"), QJsonValue(static_cast<qint64>(
                         unixSeconds(session.expiresAt)))},
                    {QStringLiteral("current"), session.sessionId == auth->sessionId},
                });
            }
            return successResponse(QJsonObject{{QStringLiteral("items"), items}});
        });

    routes.route("/user/sessions/<string>").methods(crow::HTTPMethod::DELETE)
        ([&sessions](const crow::request &request, std::string sessionId) {
            crow::response failure;
            const auto auth = userAuth(request, sessions, failure);
            if (!auth) return failure;
            const auto parsedSessionId = positiveInteger(sessionId);
            if (!parsedSessionId) return serviceError(ErrorCode::InvalidArgument);
            sessions.revokeForPrincipal(auth->principalId, *parsedSessionId);
            return successResponse();
        });

    routes.route("/user/me").methods(crow::HTTPMethod::GET)
        ([&identity, &sessions](const crow::request &request) {
            crow::response failure;
            const auto auth = userAuth(request, sessions, failure);
            if (!auth) return std::move(failure);
            return accountResult(identity.profile(auth->principalId));
        });

    routes.route("/user/me").methods(crow::HTTPMethod::PUT)
        ([&identity, &sessions](const crow::request &request) {
            crow::response failure;
            const auto auth = userAuth(request, sessions, failure);
            if (!auth) return failure;
            const auto parsed = parseJsonObject(
                request, {"nickname", "version"}, {"nickname", "version"});
            if (!parsed.object || !validStringField(*parsed.object, "nickname", 1, 20)
                || !validIntegerField(*parsed.object, "version", 1, 9007199254740991LL)) {
                return serviceError(ErrorCode::ValidationFailed);
            }
            return accountResult(identity.updateNickname(
                auth->principalId, text(*parsed.object, "nickname"),
                static_cast<std::int64_t>(parsed.object->value("version").toDouble())));
        });

    routes.route("/user/me/avatar").methods(crow::HTTPMethod::POST)
        ([&identity, &sessions, &blockingExecutor](
             const crow::request &request, crow::response &response) {
            crow::response failure;
            const auto auth = userAuth(request, sessions, failure);
            if (!auth) {
                finish(response, std::move(failure));
                return;
            }
            const std::string principal = auth->principalId;
            const std::string contentType = request.get_header_value("Content-Type");
            const std::string body = request.body;
            dispatchBlocking(request, response, blockingExecutor,
                [&identity, principal, contentType, body] {
                    auto parsed = parseAndNormalizeAvatar(contentType, body);
                    if (!parsed) return serviceError(ErrorCode::ValidationFailed);
                    const auto result = identity.updateAvatar(
                        principal, std::move(parsed->avatar));
                    if (!result.ok()) return serviceError(result.error);
                    return successResponse(QJsonObject{
                        {QStringLiteral("avatarUrl"),
                            QStringLiteral("/api/v1/user/me/avatar/content")},
                        {QStringLiteral("version"), QJsonValue(static_cast<qint64>(
                             result.value->version))},
                    });
                });
        });

    routes.route("/user/me/avatar/content").methods(crow::HTTPMethod::GET)
        ([&identity, &sessions](const crow::request &request) {
            crow::response failure;
            const auto auth = userAuth(request, sessions, failure);
            if (!auth) return failure;
            const auto profile = identity.profile(auth->principalId);
            if (!profile.ok() || !profile.value->avatar) return serviceError(ErrorCode::NotFound);
            const auto &avatar = *profile.value->avatar;
            if (request.get_header_value("If-None-Match") == avatar.etag) {
                crow::response notModified(304);
                notModified.set_header("ETag", avatar.etag);
                applyPublicSecurityHeaders(notModified);
                return notModified;
            }
            crow::response response;
            response.code = 200;
            response.body.assign(avatar.bytes.begin(), avatar.bytes.end());
            response.set_header("Content-Type", avatar.contentType);
            response.set_header("ETag", avatar.etag);
            response.set_header("Cache-Control", "private, no-cache");
            response.set_header("X-Content-Type-Options", "nosniff");
            return response;
        });

    routes.route("/user/me/credential").methods(crow::HTTPMethod::PUT)
        ([&identity, &sessions, &blockingExecutor](
             const crow::request &request, crow::response &response) {
            crow::response failure;
            const auto auth = userAuth(request, sessions, failure);
            if (!auth) {
                finish(response, std::move(failure));
                return;
            }
            const auto parsed = parseJsonObject(
                request, {"username", "currentPassword", "newPassword", "smsCode"},
                {"username", "newPassword"});
            if (!parsed.object) {
                finish(response, serviceError(ErrorCode::InvalidArgument));
                return;
            }
            auto username = text(*parsed.object, "username");
            auto currentPassword = optionalText(*parsed.object, "currentPassword");
            auto newPassword = text(*parsed.object, "newPassword");
            auto smsCode = optionalText(*parsed.object, "smsCode");
            dispatchBlocking(request, response, blockingExecutor,
                [&identity, auth = *auth, username = std::move(username),
                    currentPassword = std::move(currentPassword),
                    newPassword = std::move(newPassword),
                    smsCode = std::move(smsCode)]() mutable {
                    return accountResult(identity.updateCredential(
                        auth, std::move(username), std::move(currentPassword),
                        std::move(newPassword), std::move(smsCode),
                        std::chrono::system_clock::now()));
                });
        });

    routes.route("/user/me").methods(crow::HTTPMethod::DELETE)
        ([&identity, &sessions, &blockingExecutor](
             const crow::request &request, crow::response &response) {
            crow::response failure;
            const auto auth = userAuth(request, sessions, failure);
            if (!auth) {
                finish(response, std::move(failure));
                return;
            }
            const auto parsed = parseJsonObject(
                request, {"confirm", "password", "smsCode"}, {"confirm"});
            if (!parsed.object || !parsed.object->value("confirm").isBool()) {
                finish(response, serviceError(ErrorCode::InvalidArgument));
                return;
            }
            const bool confirmed = parsed.object->value("confirm").toBool();
            auto password = optionalText(*parsed.object, "password");
            auto smsCode = optionalText(*parsed.object, "smsCode");
            dispatchBlocking(request, response, blockingExecutor,
                [&identity, auth = *auth, confirmed, password = std::move(password),
                    smsCode = std::move(smsCode)]() mutable {
                    const auto result = identity.deleteAccount(
                        auth, confirmed, std::move(password), std::move(smsCode),
                        std::chrono::system_clock::now());
                    return result == ErrorCode::Ok
                        ? successResponse() : serviceError(result);
                });
        });
}

} // namespace ncs::server::controller
