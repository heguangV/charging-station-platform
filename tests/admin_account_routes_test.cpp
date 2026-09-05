#include "core/application/admin_account_service.h"
#include "core/application/bounded_executor.h"
#include "core/application/business_numbers.h"
#include "core/application/charge_flow_service.h"
#include "core/application/charging_repository.h"
#include "core/application/idempotency_service.h"
#include "core/application/in_memory_admin_repository.h"
#include "core/application/in_memory_user_account_repository.h"
#include "server/controller/admin_routes.h"
#include "server/controller/api_routes.h"
#include "server/server_app.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <chrono>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

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
                    const std::string& idempotencyKey = {})
{
    const auto question = url.find('?');
    crow::request request(method, url, url.substr(0, question), crow::query_string(url), {}, body,
                          1, 1, true, false, false);
    request.remote_ip_address = "127.0.0.1";
    if (!token.empty())
        request.add_header("Authorization", "Bearer " + token);
    if (!idempotencyKey.empty())
        request.add_header("Idempotency-Key", idempotencyKey);
    request.add_header("Content-Type", "application/json; charset=utf-8");
    crow::response response;
    app.handle_full(request, response);
    return response;
}

const char* kKeyM = "3cb640c6-6995-4be5-9161-f0e2c122000d";
const char* kKeyN = "3cb640c6-6995-4be5-9161-f0e2c122000e";
const char* kKeyO = "3cb640c6-6995-4be5-9161-f0e2c122000f";
const char* kKeyP = "3cb640c6-6995-4be5-9161-f0e2c1220010";
const char* kKeyQ = "3cb640c6-6995-4be5-9161-f0e2c1220011";
const char* kKeyR = "3cb640c6-6995-4be5-9161-f0e2c1220012";
const char* kKeyS = "3cb640c6-6995-4be5-9161-f0e2c1220013";
const char* kKeyT = "3cb640c6-6995-4be5-9161-f0e2c1220014";
const char* kKeyU = "3cb640c6-6995-4be5-9161-f0e2c1220015";
const char* kKeyV = "3cb640c6-6995-4be5-9161-f0e2c1220016";
const char* kKeyW = "3cb640c6-6995-4be5-9161-f0e2c1220017";
const char* kKeyX = "3cb640c6-6995-4be5-9161-f0e2c1220018";
const char* kKeyY = "3cb640c6-6995-4be5-9161-f0e2c1220019";
const char* kKeyZ = "3cb640c6-6995-4be5-9161-f0e2c122001a";
const char* kKeyAA = "3cb640c6-6995-4be5-9161-f0e2c122001b";
const char* kKeyAB = "3cb640c6-6995-4be5-9161-f0e2c122001c";
const char* kKeyAC = "3cb640c6-6995-4be5-9161-f0e2c122001d";

} // namespace

int main()
{
    using namespace ncs;
    TestRunner tests;
    core::application::SessionManager sessions;
    core::application::InMemoryUserAccountRepository accounts;
    core::application::InMemoryChargingRepository charging;
    core::application::InMemoryAdminRepository adminRepository(charging, accounts, true);
    core::application::BoundedExecutor blockingExecutor(2, 32);
    core::application::IdempotencyService idempotency;
    core::application::BusinessNumbers numbers;
    core::application::ChargeFlowService flowService(charging, accounts, accounts, numbers, 3600);
    core::application::AdminAuthService adminAuth(adminRepository, sessions);
    core::application::AdminAccountService adminAccounts(adminRepository, sessions);
    core::application::AdminUserService adminUsers(adminRepository, flowService, sessions);
    core::application::AdminStationService adminStations(adminRepository, charging, flowService,
                                                         numbers);
    core::application::AdminOpsService adminOps(adminRepository, charging, flowService, numbers);

    server::ServerApp app;
    server::controller::ApiRoutes api(app);
    server::controller::AdminRoutes adminRoutes(api, adminAuth, adminUsers, adminStations, adminOps,
                                                sessions, blockingExecutor, idempotency,
                                                adminAccounts);
    app.validate();

    const auto login = call(app, crow::HTTPMethod::POST, "/api/v1/admin/auth/login",
                            R"({"username":"admin","password":"123456","deviceId":"admin-desk"})");
    const QJsonObject loginData = envelope(login).value("data").toObject();
    const std::string adminToken =
        loginData.value(QStringLiteral("accessToken")).toString().toStdString();
    tests.check(login.code == 200 && adminToken.size() >= 43 &&
                    loginData.value("admin").toObject().value("roles").toArray().contains(
                        QStringLiteral("OPERATOR")),
                "seeded admin logs in with operator role");

    const auto reauth = call(app, crow::HTTPMethod::POST, "/api/v1/admin/auth/reauth",
                             R"({"password":"123456"})", adminToken);
    tests.check(reauth.code == 200, "reauthentication succeeds with password");

    // ---- UC-A-09 管理员账号管理 ----
    // Account management writes require OWNER plus a recent reauthentication;
    // the seeded admin session was reauthenticated at the top of this file.

    tests.check(call(app, crow::HTTPMethod::GET, "/api/v1/admin/accounts").code == 401,
                "account list rejects anonymous access");
    tests.check(call(app, crow::HTTPMethod::POST, "/api/v1/admin/accounts", "{}").code == 401,
                "account creation rejects anonymous access");
    tests.check(call(app, crow::HTTPMethod::PUT, "/api/v1/admin/accounts/2/status", "{}").code ==
                    401,
                "account status change rejects anonymous access");
    tests.check(call(app, crow::HTTPMethod::PUT, "/api/v1/admin/me/password", "{}").code == 401,
                "own password change rejects anonymous access");

    const auto accountList = call(app, crow::HTTPMethod::GET,
                                  "/api/v1/admin/accounts?page=1&pageSize=20", {}, adminToken);
    tests.check(accountList.code == 200 &&
                    envelope(accountList).value("data").toObject().value("total").toInt() >= 1,
                "OWNER lists the seeded admin account");
    bool seededAccountComplete = false;
    for (const auto& value :
         envelope(accountList).value("data").toObject().value("items").toArray())
    {
        const QJsonObject item = value.toObject();
        if (item.value("username").toString() == QStringLiteral("admin") &&
            item.value("status").toInt() == 1 && !item.value("mustChangePassword").toBool() &&
            item.value("version").toInt() >= 1 &&
            item.value("roles").toArray().contains(QStringLiteral("OWNER")))
        {
            seededAccountComplete = true;
        }
    }
    tests.check(seededAccountComplete, "seeded admin appears in the list with full account fields");
    tests.check(accountList.body.find("passwordHash") == std::string::npos &&
                    accountList.body.find("123456") == std::string::npos,
                "account list never leaks password hashes or secrets");

    tests.check(
        call(app, crow::HTTPMethod::POST, "/api/v1/admin/accounts",
             R"({"username":"ops_nokey","password":"ncs-Initial-2026","reason":"缺少幂等键"})",
             adminToken)
                .code == 400,
        "account creation without an idempotency key is rejected");
    tests.check(call(app, crow::HTTPMethod::PUT, "/api/v1/admin/accounts/2/status",
                     R"({"status":0,"reason":"缺少幂等键","version":1})", adminToken)
                        .code == 400,
                "account status change without an idempotency key is rejected");

    tests.check(call(app, crow::HTTPMethod::POST, "/api/v1/admin/accounts",
                     R"({"username":"ab","password":"ncs-Initial-2026","reason":"账号名过短"})",
                     adminToken, kKeyO)
                        .code == 422,
                "account names shorter than three characters are rejected");
    tests.check(call(app, crow::HTTPMethod::POST, "/api/v1/admin/accounts",
                     R"({"username":"ops wang","password":"ncs-Initial-2026","reason":"非法字符"})",
                     adminToken, kKeyP)
                        .code == 422,
                "account names with illegal characters are rejected");
    tests.check(call(app, crow::HTTPMethod::POST, "/api/v1/admin/accounts",
                     R"({"username":"ops_ok1","password":"123456789","reason":"口令过短"})",
                     adminToken, kKeyQ)
                        .code == 422,
                "account passwords shorter than ten characters are rejected");
    const auto longPassword = call(app, crow::HTTPMethod::POST, "/api/v1/admin/accounts",
                                   "{\"username\":\"ops_ok2\",\"password\":\"" +
                                       std::string(129, 'a') + "\",\"reason\":\"口令过长\"}",
                                   adminToken, kKeyR);
    tests.check(longPassword.code == 422, "account passwords over 128 characters are rejected");
    tests.check(call(app, crow::HTTPMethod::POST, "/api/v1/admin/accounts",
                     R"({"username":"ops_ok3","password":"ncs-Initial-2026"})", adminToken, kKeyS)
                        .code == 422,
                "account creation without a reason is rejected");
    tests.check(call(app, crow::HTTPMethod::POST, "/api/v1/admin/accounts",
                     R"({"username":"ops_ok4","password":"ncs-Initial-2026","reason":"x"})",
                     adminToken, kKeyT)
                        .code == 422,
                "account creation with a too-short reason is rejected");

    const auto created =
        call(app, crow::HTTPMethod::POST, "/api/v1/admin/accounts",
             R"({"username":"ops_wang","password":"ncs-Initial-2026","reason":"新入职运营专员"})",
             adminToken, kKeyM);
    tests.check(created.code == 201, "OWNER creates an operator account");
    const QJsonObject createdData = envelope(created).value("data").toObject();
    const std::string opsId = std::to_string(createdData.value(QStringLiteral("id")).toInt());
    tests.check(createdData.value("username").toString() == QStringLiteral("ops_wang") &&
                    createdData.value("status").toInt() == 1 &&
                    createdData.value("mustChangePassword").toBool() &&
                    createdData.value("version").toInt() == 1 &&
                    createdData.value("roles").toArray().size() == 1 &&
                    createdData.value("roles").toArray().contains(QStringLiteral("OPERATOR")) &&
                    !createdData.value("roles").toArray().contains(QStringLiteral("OWNER")),
                "created account is an enabled OPERATOR flagged for a password change");
    tests.check(created.body.find("ncs-Initial-2026") == std::string::npos,
                "creation response never echoes the password");

    tests.check(call(app, crow::HTTPMethod::POST, "/api/v1/admin/accounts",
                     R"({"username":"ops_wang","password":"ncs-Initial-2026","reason":"重复账号"})",
                     adminToken, kKeyN)
                        .code == 409,
                "duplicate account names return ALREADY_EXISTS");

    const auto secondLogin =
        call(app, crow::HTTPMethod::POST, "/api/v1/admin/auth/login",
             R"({"username":"admin","password":"123456","deviceId":"admin-desk-2"})");
    const std::string adminToken2 = envelope(secondLogin)
                                        .value("data")
                                        .toObject()
                                        .value(QStringLiteral("accessToken"))
                                        .toString()
                                        .toStdString();
    tests.check(secondLogin.code == 200 && !adminToken2.empty(),
                "a second OWNER session logs in without reauthentication");
    tests.check(call(app, crow::HTTPMethod::POST, "/api/v1/admin/accounts",
                     R"({"username":"ops_wang","password":"ncs-Initial-2026","reason":"重复账号"})",
                     adminToken2, kKeyV)
                        .code == 401,
                "account creation demands recent reauthentication");
    tests.check(call(app, crow::HTTPMethod::POST, "/api/v1/admin/auth/reauth",
                     R"({"password":"123456"})", adminToken2)
                        .code == 200,
                "the second OWNER session reauthenticates");
    tests.check(call(app, crow::HTTPMethod::POST, "/api/v1/admin/accounts",
                     R"({"username":"ops_wang","password":"ncs-Initial-2026","reason":"重复账号"})",
                     adminToken2, kKeyW)
                        .code == 409,
                "a reauthenticated OWNER reaches the duplicate-name check");

    const auto opsLogin =
        call(app, crow::HTTPMethod::POST, "/api/v1/admin/auth/login",
             R"({"username":"ops_wang","password":"ncs-Initial-2026","deviceId":"ops-desk"})");
    const QJsonObject opsLoginData = envelope(opsLogin).value("data").toObject();
    const std::string opsToken =
        opsLoginData.value(QStringLiteral("accessToken")).toString().toStdString();
    tests.check(opsLogin.code == 200 &&
                    opsLoginData.value("admin").toObject().value("mustChangePassword").toBool(),
                "initial login of the new account carries the change-required flag");
    tests.check(call(app, crow::HTTPMethod::GET, "/api/v1/admin/accounts", {}, opsToken).code ==
                    403,
                "non-OWNER admins cannot list accounts");
    tests.check(call(app, crow::HTTPMethod::POST, "/api/v1/admin/accounts",
                     R"({"username":"ops_two","password":"ncs-Initial-2026","reason":"越权"})",
                     opsToken, kKeyU)
                        .code == 403,
                "non-OWNER admins cannot create accounts");
    tests.check(call(app, crow::HTTPMethod::PUT, "/api/v1/admin/accounts/" + opsId + "/status",
                     R"({"status":0,"reason":"越权","version":1})", opsToken, kKeyX)
                        .code == 403,
                "non-OWNER admins cannot change account status");
    tests.check(call(app, crow::HTTPMethod::GET, "/api/v1/admin/audit-logs", {}, opsToken).code ==
                    403,
                "non-OWNER admins cannot read audit logs");

    const auto opsSecondLogin =
        call(app, crow::HTTPMethod::POST, "/api/v1/admin/auth/login",
             R"({"username":"ops_wang","password":"ncs-Initial-2026","deviceId":"ops-desk-2"})");
    const std::string opsSecondToken = envelope(opsSecondLogin)
                                           .value("data")
                                           .toObject()
                                           .value(QStringLiteral("accessToken"))
                                           .toString()
                                           .toStdString();
    tests.check(opsSecondLogin.code == 200,
                "the operator holds a second session on another device");

    tests.check(call(app, crow::HTTPMethod::PUT, "/api/v1/admin/me/password",
                     R"({"currentPassword":"wrong-current","newPassword":"ncs-Next-Password-2"})",
                     opsToken)
                        .code == 401,
                "a wrong current password cannot change the account password");
    tests.check(call(app, crow::HTTPMethod::PUT, "/api/v1/admin/me/password",
                     R"({"currentPassword":"ncs-Initial-2026","newPassword":"123456789"})",
                     opsToken)
                        .code == 422,
                "weak new passwords are rejected when changing own password");
    const auto changed = call(
        app, crow::HTTPMethod::PUT, "/api/v1/admin/me/password",
        R"({"currentPassword":"ncs-Initial-2026","newPassword":"ncs-Next-Password-2"})", opsToken);
    tests.check(
        changed.code == 200 &&
            !envelope(changed).value("data").toObject().value("mustChangePassword").toBool(),
        "changing own password clears the must-change-password flag");
    const int versionAfterPasswordChange =
        envelope(changed).value("data").toObject().value("version").toInt();
    tests.check(
        call(app, crow::HTTPMethod::GET, "/api/v1/admin/users?status=1", {}, opsToken).code == 200,
        "the current session survives the own password change");
    tests.check(
        call(app, crow::HTTPMethod::GET, "/api/v1/admin/users?status=1", {}, opsSecondToken).code ==
            401,
        "other sessions of the operator are revoked by the password change");
    tests.check(
        call(app, crow::HTTPMethod::POST, "/api/v1/admin/auth/login",
             R"({"username":"ops_wang","password":"ncs-Initial-2026","deviceId":"ops-desk-3"})")
                .code == 401,
        "the old password stops working after the change");
    const auto freshLogin =
        call(app, crow::HTTPMethod::POST, "/api/v1/admin/auth/login",
             R"({"username":"ops_wang","password":"ncs-Next-Password-2","deviceId":"ops-desk-3"})");
    tests.check(freshLogin.code == 200 && !envelope(freshLogin)
                                               .value("data")
                                               .toObject()
                                               .value("admin")
                                               .toObject()
                                               .value("mustChangePassword")
                                               .toBool(),
                "the new password logs in without the change-required flag");

    const auto staleDisable =
        call(app, crow::HTTPMethod::PUT, "/api/v1/admin/accounts/" + opsId + "/status",
             R"({"status":0,"reason":"该管理员离岗","version":999})", adminToken, kKeyY);
    tests.check(staleDisable.code == 409 && envelope(staleDisable).value("code").toInt() == 22,
                "a stale account version conflicts on status change");
    tests.check(call(app, crow::HTTPMethod::PUT, "/api/v1/admin/accounts/99999/status",
                     R"({"status":0,"reason":"账号不存在","version":1})", adminToken, kKeyAA)
                        .code == 404,
                "status change for a missing account returns not found");
    tests.check(call(app, crow::HTTPMethod::PUT, "/api/v1/admin/accounts/1/status",
                     R"({"status":0,"reason":"误操作","version":1})", adminToken, kKeyAB)
                        .code == 422,
                "an OWNER cannot disable their own account");
    const auto disabled =
        call(app, crow::HTTPMethod::PUT, "/api/v1/admin/accounts/" + opsId + "/status",
             "{\"status\":0,\"reason\":\"该管理员离岗\",\"version\":" +
                 std::to_string(versionAfterPasswordChange) + "}",
             adminToken, kKeyZ);
    tests.check(disabled.code == 200 &&
                    envelope(disabled).value("data").toObject().value("status").toInt() == 0,
                "disabling an account updates its status");
    const int versionAfterDisable =
        envelope(disabled).value("data").toObject().value("version").toInt();
    tests.check(
        call(app, crow::HTTPMethod::GET, "/api/v1/admin/users?status=1", {}, opsToken).code == 401,
        "disabling an account revokes its sessions immediately");
    tests.check(
        call(app, crow::HTTPMethod::POST, "/api/v1/admin/auth/login",
             R"({"username":"ops_wang","password":"ncs-Next-Password-2","deviceId":"ops-desk-4"})")
                .code == 401,
        "a disabled account cannot log in with its password");
    bool disabledReflected = false;
    const auto listWhileDisabled = call(
        app, crow::HTTPMethod::GET, "/api/v1/admin/accounts?page=1&pageSize=20", {}, adminToken);
    for (const auto& value :
         envelope(listWhileDisabled).value("data").toObject().value("items").toArray())
    {
        if (value.toObject().value("username").toString() == QStringLiteral("ops_wang") &&
            value.toObject().value("status").toInt() == 0)
        {
            disabledReflected = true;
        }
    }
    tests.check(listWhileDisabled.code == 200 && disabledReflected,
                "the account list reflects the disabled status");
    const auto enabled =
        call(app, crow::HTTPMethod::PUT, "/api/v1/admin/accounts/" + opsId + "/status",
             "{\"status\":1,\"reason\":\"离岗原因解除\",\"version\":" +
                 std::to_string(versionAfterDisable) + "}",
             adminToken, kKeyAC);
    tests.check(enabled.code == 200 &&
                    envelope(enabled).value("data").toObject().value("status").toInt() == 1,
                "enabling restores the account");
    tests.check(
        call(app, crow::HTTPMethod::POST, "/api/v1/admin/auth/login",
             R"({"username":"ops_wang","password":"ncs-Next-Password-2","deviceId":"ops-desk-5"})")
                .code == 200,
        "the enabled account logs in with the unchanged password");

    const auto createdAudit = call(app, crow::HTTPMethod::GET,
                                   "/api/v1/admin/audit-logs?action=ADMIN_CREATED", {}, adminToken);
    tests.check(
        createdAudit.code == 200 &&
            envelope(createdAudit).value("data").toObject().value("items").toArray().size() >= 1,
        "audit trail records account creation");
    const auto changedAudit =
        call(app, crow::HTTPMethod::GET, "/api/v1/admin/audit-logs?action=ADMIN_PASSWORD_CHANGED",
             {}, adminToken);
    tests.check(
        changedAudit.code == 200 &&
            envelope(changedAudit).value("data").toObject().value("items").toArray().size() >= 1,
        "audit trail records own password changes");
    const auto disabledAudit =
        call(app, crow::HTTPMethod::GET, "/api/v1/admin/audit-logs?action=ADMIN_DISABLED", {},
             adminToken);
    tests.check(
        disabledAudit.code == 200 &&
            envelope(disabledAudit).value("data").toObject().value("items").toArray().size() >= 1,
        "audit trail records account disabling");
    const auto enabledAudit = call(app, crow::HTTPMethod::GET,
                                   "/api/v1/admin/audit-logs?action=ADMIN_ENABLED", {}, adminToken);
    tests.check(
        enabledAudit.code == 200 &&
            envelope(enabledAudit).value("data").toObject().value("items").toArray().size() >= 1,
        "audit trail records account enabling");

    // Lockout coordination: failure counters are independent per account.
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        call(app, crow::HTTPMethod::POST, "/api/v1/admin/auth/login",
             R"({"username":"ops_wang","password":"wrong-password","deviceId":"ops-desk-lock"})");
    }
    tests.check(
        call(
            app, crow::HTTPMethod::POST, "/api/v1/admin/auth/login",
            R"({"username":"ops_wang","password":"ncs-Next-Password-2","deviceId":"ops-desk-lock"})")
                .code == 429,
        "five consecutive failures lock the new admin account");
    tests.check(call(app, crow::HTTPMethod::POST, "/api/v1/admin/auth/login",
                     R"({"username":"admin","password":"123456","deviceId":"admin-desk"})")
                        .code == 200,
                "the demo admin login is unaffected by another account's lockout");

    return tests.result();
}
