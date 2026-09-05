#include "core/application/bounded_executor.h"
#include "core/application/in_memory_user_account_repository.h"
#include "core/application/user_identity_service.h"
#include "server/controller/api_routes.h"
#include "server/controller/user_identity_routes.h"
#include "server/server_app.h"

#include <QBuffer>
#include <QColor>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <chrono>
#include <iostream>
#include <string_view>

#if defined(_WIN32) && defined(DELETE)
#undef DELETE
#endif

namespace
{

class TestRunner final
{
  public:
    void check(const bool condition, const std::string_view message)
    {
        if (!condition)
        {
            std::cerr << "FAIL: " << message << '\n';
            ++failures_;
        }
    }
    int result() const
    {
        return failures_ == 0 ? 0 : 1;
    }

  private:
    int failures_ = 0;
};

QJsonObject envelope(const crow::response& response)
{
    return QJsonDocument::fromJson(QByteArray::fromStdString(response.body)).object();
}

crow::response call(ncs::server::ServerApp& app, const crow::HTTPMethod method, std::string url,
                    std::string body = {}, std::string token = {},
                    std::string contentType = "application/json; charset=utf-8")
{
    crow::request request;
    request.method = method;
    request.url = std::move(url);
    request.raw_url = request.url;
    request.remote_ip_address = "127.0.0.1";
    request.body = std::move(body);
    if (!contentType.empty())
        request.add_header("Content-Type", std::move(contentType));
    if (!token.empty())
        request.add_header("Authorization", "Bearer " + token);
    crow::response response;
    app.handle_full(request, response);
    return response;
}

std::string issueCode(ncs::server::ServerApp& app, const std::string_view phone,
                      const std::string_view purpose)
{
    const auto response = call(app, crow::HTTPMethod::POST, "/api/v1/user/auth/sms/code",
                               "{\"phone\":\"" + std::string(phone) + "\",\"purpose\":\"" +
                                   std::string(purpose) + "\"}");
    return envelope(response)
        .value(QStringLiteral("data"))
        .toObject()
        .value(QStringLiteral("developmentCode"))
        .toString()
        .toStdString();
}

std::string pngMultipart(std::string& contentType)
{
    QImage image(2, 2, QImage::Format_ARGB32);
    image.fill(Qt::red);
    image.setText(QStringLiteral("Author"), QStringLiteral("secret-metadata"));
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    const std::string boundary = "ncs-test-boundary";
    contentType = "multipart/form-data; boundary=" + boundary;
    std::string body = "--" + boundary +
                       "\r\nContent-Disposition: form-data; name=\"file\"; "
                       "filename=\"avatar.png\""
                       "\r\nContent-Type: image/png\r\n\r\n";
    body.append(png.constData(), static_cast<std::size_t>(png.size()));
    body += "\r\n--" + boundary + "--\r\n";
    return body;
}

std::string imageMultipart(const QImage& image, std::string& contentType,
                           const std::string& boundaryWithParameters = "boundary=ncs-test-boundary")
{
    QByteArray encoded;
    QBuffer buffer(&encoded);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    contentType = "multipart/form-data; " + boundaryWithParameters;
    std::string body = "--ncs-test-boundary" + std::string("\r\nContent-Disposition: form-data; "
                                                           "name=\"file\"; filename=\"avatar.png\""
                                                           "\r\nContent-Type: image/png\r\n\r\n");
    body.append(encoded.constData(), static_cast<std::size_t>(encoded.size()));
    body += "\r\n--ncs-test-boundary--\r\n";
    return body;
}

} // namespace

int main()
{
    using namespace ncs;
    TestRunner tests;
    core::application::SessionManager sessions;
    core::application::VerificationCodeService codes(true);
    core::application::InMemoryUserAccountRepository accounts;
    core::application::UserIdentityService identity(accounts, sessions, codes);
    core::application::BoundedExecutor blockingExecutor(2, 32);
    server::ServerApp app;
    server::controller::ApiRoutes api(app);
    server::controller::UserIdentityRoutes routes(api, identity, sessions, blockingExecutor, true);
    app.validate();

    tests.check(call(app, crow::HTTPMethod::POST, "/api/v1/user/auth/sms/code",
                     R"({"phone":"23800138000","purpose":"REGISTER"})")
                        .code == 400,
                "phone validation rejects an eleven-digit number outside the "
                "mobile namespace");
    const std::string registerCode = issueCode(app, "13800138000", "REGISTER");
    tests.check(registerCode.size() == 6, "development code endpoint returns six-digit code");
    const auto registerResponse = call(app, crow::HTTPMethod::POST, "/api/v1/user/auth/register",
                                       "{\"username\":\"driver_001\",\"phone\":\"13800138000\","
                                       "\"password\":\"example-password\",\"smsCode\":\"" +
                                           registerCode + "\",\"deviceId\":\"desktop-a1\"}");
    const QJsonObject registerEnvelope = envelope(registerResponse);
    const QJsonObject registerData = registerEnvelope.value(QStringLiteral("data")).toObject();
    const std::string token =
        registerData.value(QStringLiteral("accessToken")).toString().toStdString();
    tests.check(registerResponse.code == 201 &&
                    registerEnvelope.value(QStringLiteral("code")).toInt() == 0 &&
                    token.size() >= 43,
                "registration creates account and returns token once");
    tests.check(registerData.value(QStringLiteral("user"))
                        .toObject()
                        .value(QStringLiteral("phoneMasked"))
                        .toString() == QStringLiteral("138****8000"),
                "registration response masks phone");

    const auto badLogin =
        call(app, crow::HTTPMethod::POST, "/api/v1/user/auth/login/password",
             R"({"loginName":"missing","password":"wrong-password","deviceId":"desktop-bad"})");
    tests.check(badLogin.code == 401,
                "unknown account and wrong password share unauthorized response");
    const auto passwordLogin =
        call(app, crow::HTTPMethod::POST, "/api/v1/user/auth/login/password",
             R"({"loginName":"driver_001","password":"example-password","deviceId":"desktop-a2"})");
    const QJsonObject passwordData =
        envelope(passwordLogin).value(QStringLiteral("data")).toObject();
    const std::string secondToken =
        passwordData.value(QStringLiteral("accessToken")).toString().toStdString();
    const auto secondSession = passwordData.value(QStringLiteral("sessionId")).toInteger();
    tests.check(passwordLogin.code == 200 && !secondToken.empty() &&
                    passwordData.value(QStringLiteral("sessionId")).isDouble() && secondSession > 0,
                "password login succeeds with numeric session id");

    const auto resetUnknown = call(app, crow::HTTPMethod::POST, "/api/v1/user/auth/sms/code",
                                   R"({"phone":"19999999999","purpose":"RESET_PASSWORD"})");
    const auto resetKnown = call(app, crow::HTTPMethod::POST, "/api/v1/user/auth/sms/code",
                                 R"({"phone":"13800138000","purpose":"RESET_PASSWORD"})");
    tests.check(resetUnknown.code == 200 && resetKnown.code == 200 &&
                    envelope(resetUnknown)
                        .value("data")
                        .toObject()
                        .contains(QStringLiteral("developmentCode")) &&
                    envelope(resetKnown)
                        .value("data")
                        .toObject()
                        .contains(QStringLiteral("developmentCode")),
                "reset code response does not reveal phone registration");

    const std::string digitCode = issueCode(app, "13800138005", "REGISTER");
    const auto digitRegister = call(app, crow::HTTPMethod::POST, "/api/v1/user/auth/register",
                                    "{\"username\":\"13800138055\",\"phone\":\"13800138005\","
                                    "\"password\":\"example-password\",\"smsCode\":\"" +
                                        digitCode + "\",\"deviceId\":\"desktop-digit\"}");
    tests.check(digitRegister.code == 422, "digits-only username is rejected");

    const auto me = call(app, crow::HTTPMethod::GET, "/api/v1/user/me", {}, token, {});
    const QJsonObject meData = envelope(me).value(QStringLiteral("data")).toObject();
    tests.check(me.code == 200 && meData.value(QStringLiteral("version")).toInt() == 1,
                "profile is authorized");
    tests.check(me.body.find("13800138000") == std::string::npos &&
                    me.body.find("password") == std::string::npos,
                "profile omits full phone and credential digest");

    const auto unknownField = call(app, crow::HTTPMethod::PUT, "/api/v1/user/me",
                                   R"({"nickname":"new","version":1,"admin":true})", token);
    tests.check(unknownField.code == 422, "profile update rejects unknown fields");
    const auto updated = call(app, crow::HTTPMethod::PUT, "/api/v1/user/me",
                              R"({"nickname":"新昵称","version":1})", token);
    tests.check(updated.code == 200 && envelope(updated)
                                               .value(QStringLiteral("data"))
                                               .toObject()
                                               .value(QStringLiteral("version"))
                                               .toInt() == 2,
                "nickname update increments version");
    const auto controlNickname = call(app, crow::HTTPMethod::PUT, "/api/v1/user/me",
                                      R"({"nickname":"bad\nname","version":2})", token);
    tests.check(controlNickname.code == 422, "nickname rejects control characters");
    const auto conflict = call(app, crow::HTTPMethod::PUT, "/api/v1/user/me",
                               R"({"nickname":"旧版本","version":1})", token);
    tests.check(conflict.code == 409 && envelope(conflict).value("code").toInt() == 22,
                "stale profile version returns VERSION_CONFLICT");

    const auto sessionList =
        call(app, crow::HTTPMethod::GET, "/api/v1/user/sessions", {}, token, {});
    const QJsonArray sessionItems =
        envelope(sessionList).value("data").toObject().value("items").toArray();
    tests.check(sessionItems.size() == 2, "session list returns both active terminals");
    const auto malformedSession =
        call(app, crow::HTTPMethod::DELETE, "/api/v1/user/sessions/not-a-number", {}, token, {});
    tests.check(malformedSession.code == 400, "session route rejects a malformed numeric id");
    const auto revoke =
        call(app, crow::HTTPMethod::DELETE,
             "/api/v1/user/sessions/" + std::to_string(secondSession), {}, token, {});
    tests.check(revoke.code == 200, "user can revoke own session");
    const auto revokedAccess =
        call(app, crow::HTTPMethod::GET, "/api/v1/user/me", {}, secondToken, {});
    tests.check(revokedAccess.code == 401, "revoked session loses access immediately");

    const std::string invalidBoundary = "invalid-avatar";
    const std::string invalidMultipart =
        "--" + invalidBoundary +
        "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"fake.png\""
        "\r\nContent-Type: image/png\r\n\r\nnot-an-image\r\n--" +
        invalidBoundary + "--\r\n";
    const auto invalidAvatar =
        call(app, crow::HTTPMethod::POST, "/api/v1/user/me/avatar", invalidMultipart, token,
             "multipart/form-data; boundary=" + invalidBoundary);
    tests.check(invalidAvatar.code == 422, "forged image content is rejected");
    std::string multipartType;
    const std::string multipart = pngMultipart(multipartType);
    const auto avatarUpload = call(app, crow::HTTPMethod::POST, "/api/v1/user/me/avatar", multipart,
                                   token, multipartType);
    tests.check(avatarUpload.code == 200 &&
                    envelope(avatarUpload).value("data").toObject().value("version").toInt() == 3,
                "valid image is normalized and stored");
    auto avatar = call(app, crow::HTTPMethod::GET, "/api/v1/user/me/avatar/content", {}, token, {});
    tests.check(avatar.code == 200 && avatar.get_header_value("Content-Type") == "image/png" &&
                    !avatar.get_header_value("ETag").empty() &&
                    avatar.body.find("secret-metadata") == std::string::npos,
                "avatar returns normalized MIME and ETag without source metadata");
    crow::request conditional;
    conditional.method = crow::HTTPMethod::GET;
    conditional.url = "/api/v1/user/me/avatar/content";
    conditional.raw_url = conditional.url;
    conditional.remote_ip_address = "127.0.0.1";
    conditional.add_header("Authorization", "Bearer " + token);
    conditional.add_header("If-None-Match", avatar.get_header_value("ETag"));
    crow::response conditionalResponse;
    app.handle_full(conditional, conditionalResponse);
    tests.check(conditionalResponse.code == 304 && conditionalResponse.body.empty(),
                "avatar supports conditional GET");

    std::string parameterizedType;
    const std::string parameterizedMultipart =
        imageMultipart(QImage(2, 2, QImage::Format_ARGB32), parameterizedType,
                       "boundary=ncs-test-boundary; charset=utf-8");
    const auto parameterizedAvatar = call(app, crow::HTTPMethod::POST, "/api/v1/user/me/avatar",
                                          parameterizedMultipart, token, parameterizedType);
    tests.check(parameterizedAvatar.code == 200,
                "multipart boundary tolerates trailing parameters and quoting");

    QImage gradient(2048, 2048, QImage::Format_RGB888);
    for (int y = 0; y < gradient.height(); ++y)
    {
        for (int x = 0; x < gradient.width(); ++x)
        {
            const int value = (x + y) % 256;
            gradient.setPixel(x, y, qRgb(value, value, value));
        }
    }
    std::string gradientType;
    const std::string gradientMultipart = imageMultipart(gradient, gradientType);
    const auto gradientUpload = call(app, crow::HTTPMethod::POST, "/api/v1/user/me/avatar",
                                     gradientMultipart, token, gradientType);
    tests.check(gradientUpload.code == 200, "oversized avatar is downscaled and stored");
    const auto gradientContent =
        call(app, crow::HTTPMethod::GET, "/api/v1/user/me/avatar/content", {}, token, {});
    const QImage storedGradient = QImage::fromData(QByteArray::fromStdString(gradientContent.body));
    tests.check(gradientContent.code == 200 && storedGradient.width() == 1024 &&
                    storedGradient.height() == 1024,
                "stored avatar is capped at 1024 pixels");

    QImage noise(1024, 1024, QImage::Format_RGB888);
    quint32 randomState = 0x9e3779b9u;
    for (int y = 0; y < noise.height(); ++y)
    {
        for (int x = 0; x < noise.width(); ++x)
        {
            randomState = randomState * 1664525u + 1013904223u;
            noise.setPixel(
                x, y,
                qRgb((randomState >> 16) & 0xff, (randomState >> 8) & 0xff, randomState & 0xff));
        }
    }
    std::string noiseType;
    const std::string noiseMultipart = imageMultipart(noise, noiseType);
    const auto noiseUpload = call(app, crow::HTTPMethod::POST, "/api/v1/user/me/avatar",
                                  noiseMultipart, token, noiseType);
    tests.check(noiseUpload.code == 422, "incompressible normalized avatar is rejected");

    const auto otherBeforeCredential =
        call(app, crow::HTTPMethod::POST, "/api/v1/user/auth/login/password",
             R"({"loginName":"driver_001","password":"example-password","deviceId":"desktop-a4"})");
    const std::string otherBeforeToken = envelope(otherBeforeCredential)
                                             .value("data")
                                             .toObject()
                                             .value("accessToken")
                                             .toString()
                                             .toStdString();
    const auto credential = call(
        app, crow::HTTPMethod::PUT, "/api/v1/user/me/credential",
        R"({"username":"driver_new","currentPassword":"example-password","newPassword":"new-example-password","smsCode":null})",
        token);
    tests.check(credential.code == 200, "credential update verifies current password");
    const auto revokedByCredential =
        call(app, crow::HTTPMethod::GET, "/api/v1/user/me", {}, otherBeforeToken, {});
    tests.check(revokedByCredential.code == 401, "credential update revokes other sessions");
    const auto thirdLogin = call(
        app, crow::HTTPMethod::POST, "/api/v1/user/auth/login/password",
        R"({"loginName":"driver_new","password":"new-example-password","deviceId":"desktop-a3"})");
    const std::string thirdToken =
        envelope(thirdLogin).value("data").toObject().value("accessToken").toString().toStdString();
    const auto logout =
        call(app, crow::HTTPMethod::POST, "/api/v1/user/auth/logout", {}, thirdToken, {});
    const auto repeatedLogout =
        call(app, crow::HTTPMethod::POST, "/api/v1/user/auth/logout", {}, thirdToken, {});
    tests.check(logout.code == 200 && repeatedLogout.code == 200, "logout is idempotent");

    const auto adminSession =
        sessions.issue("9", "admin-console", core::application::TokenKind::Administrator,
                       {core::application::Role::Operator}, std::chrono::system_clock::now(),
                       std::chrono::hours(1));
    const auto adminLogout = call(app, crow::HTTPMethod::POST, "/api/v1/user/auth/logout", {},
                                  adminSession ? adminSession->accessToken : std::string(), {});
    tests.check(adminLogout.code == 200, "user logout responds uniformly to non-user tokens");
    tests.check(
        adminSession &&
            sessions.authenticate(adminSession->accessToken, std::chrono::system_clock::now())
                .has_value(),
        "user logout does not revoke non-user tokens");

    const std::string smsCode = issueCode(app, "13800138001", "LOGIN");
    const auto smsLogin = call(app, crow::HTTPMethod::POST, "/api/v1/user/auth/login/sms",
                               "{\"phone\":\"13800138001\",\"smsCode\":\"" + smsCode +
                                   "\",\"deviceId\":\"desktop-sms\"}");
    tests.check(smsLogin.code == 200 && envelope(smsLogin)
                                                .value("data")
                                                .toObject()
                                                .value("user")
                                                .toObject()
                                                .value("nickname")
                                                .toString() == QStringLiteral("用户8001"),
                "SMS login automatically creates account with default nickname");

    const auto deletion =
        call(app, crow::HTTPMethod::DELETE, "/api/v1/user/me",
             R"({"confirm":true,"password":"new-example-password","smsCode":null})", token);
    tests.check(deletion.code == 200, "confirmed account deletion succeeds");
    const auto afterDeletion = call(app, crow::HTTPMethod::GET, "/api/v1/user/me", {}, token, {});
    tests.check(afterDeletion.code == 401, "account deletion revokes all sessions");
    const auto deletedAvatar =
        call(app, crow::HTTPMethod::GET, "/api/v1/user/me/avatar/content", {}, token, {});
    tests.check(deletedAvatar.code == 401, "deleted avatar is no longer accessible");

    core::application::PasswordHasher hasher;
    core::application::UserAccount frozen;
    frozen.username = "frozen_user";
    frozen.phone = "13800138002";
    frozen.nickname = "冻结用户";
    frozen.passwordHash = hasher.hash("frozen-password");
    frozen.status = 0;
    frozen.registeredAt = 1;
    accounts.create(frozen);
    const auto frozenLogin = call(
        app, crow::HTTPMethod::POST, "/api/v1/user/auth/login/password",
        R"({"loginName":"frozen_user","password":"frozen-password","deviceId":"frozen-device"})");
    tests.check(frozenLogin.code == 403 && envelope(frozenLogin).value("code").toInt() == 6,
                "frozen user cannot log in");

    core::application::UserAccount active;
    active.username = "active_user";
    active.phone = "13800138003";
    active.nickname = "流程用户";
    active.passwordHash = hasher.hash("active-password");
    active.hasActiveFlow = true;
    active.registeredAt = 1;
    accounts.create(active);
    const auto activeLogin = call(
        app, crow::HTTPMethod::POST, "/api/v1/user/auth/login/password",
        R"({"loginName":"active_user","password":"active-password","deviceId":"active-device"})");
    const std::string activeToken = envelope(activeLogin)
                                        .value("data")
                                        .toObject()
                                        .value("accessToken")
                                        .toString()
                                        .toStdString();
    const auto blockedDeletion =
        call(app, crow::HTTPMethod::DELETE, "/api/v1/user/me",
             R"({"confirm":true,"password":"active-password","smsCode":null})", activeToken);
    tests.check(blockedDeletion.code == 409 && envelope(blockedDeletion).value("code").toInt() == 9,
                "active charging flow blocks account deletion");
    core::application::UserAccount shouldRemainActive;
    tests.check(accounts.anonymize(active.id, shouldRemainActive) ==
                        core::application::AccountWriteResult::ActiveFlowExists &&
                    accounts.findById(active.id) && !accounts.findById(active.id)->deleted,
                "repository atomically refuses anonymization with an active flow");
    return tests.result();
}
