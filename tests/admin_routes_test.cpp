#include "core/application/business_numbers.h"
#include "core/application/bounded_executor.h"
#include "core/application/charge_flow_service.h"
#include "core/application/charging_repository.h"
#include "core/application/idempotency_service.h"
#include "core/application/in_memory_admin_repository.h"
#include "core/application/in_memory_user_account_repository.h"
#include "core/application/user_identity_service.h"
#include "core/application/wallet_service.h"
#include "server/controller/admin_routes.h"
#include "server/controller/api_routes.h"
#include "server/controller/flow_routes.h"
#include "server/controller/user_identity_routes.h"
#include "server/controller/wallet_routes.h"
#include "server/server_app.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <chrono>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace {

class TestRunner final {
public:
  void check(const bool condition, const std::string_view message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      ++failures_;
    }
  }
  int result() const { return failures_ == 0 ? 0 : 1; }

private:
  int failures_ = 0;
};

QJsonObject envelope(const crow::response &response) {
  return QJsonDocument::fromJson(QByteArray::fromStdString(response.body))
      .object();
}

crow::response call(ncs::server::ServerApp &app, const crow::HTTPMethod method,
                    std::string url, std::string body = {},
                    std::string token = {},
                    const std::string &idempotencyKey = {}) {
  const auto question = url.find('?');
  crow::request request(method, url, url.substr(0, question),
                        crow::query_string(url), {}, body, 1, 1, true, false,
                        false);
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

std::string smsCode(ncs::server::ServerApp &app, const std::string_view phone,
                    const std::string_view purpose) {
  const auto response = call(
      app, crow::HTTPMethod::POST, "/api/v1/user/auth/sms/code",
      "{\"phone\":\"" + std::string(phone) + "\",\"purpose\":\"" +
          std::string(purpose) + "\"}");
  return envelope(response)
      .value(QStringLiteral("data"))
      .toObject()
      .value(QStringLiteral("developmentCode"))
      .toString()
      .toStdString();
}

const char *kKeyA = "3cb640c6-6995-4be5-9161-f0e2c1220001";
const char *kKeyB = "3cb640c6-6995-4be5-9161-f0e2c1220002";
const char *kKeyC = "3cb640c6-6995-4be5-9161-f0e2c1220003";
const char *kKeyD = "3cb640c6-6995-4be5-9161-f0e2c1220004";
const char *kKeyE = "3cb640c6-6995-4be5-9161-f0e2c1220005";
const char *kKeyF = "3cb640c6-6995-4be5-9161-f0e2c1220006";
const char *kKeyG = "3cb640c6-6995-4be5-9161-f0e2c1220007";
const char *kKeyH = "3cb640c6-6995-4be5-9161-f0e2c1220008";
const char *kKeyI = "3cb640c6-6995-4be5-9161-f0e2c1220009";
const char *kKeyJ = "3cb640c6-6995-4be5-9161-f0e2c122000a";
const char *kKeyK = "3cb640c6-6995-4be5-9161-f0e2c122000b";
const char *kKeyL = "3cb640c6-6995-4be5-9161-f0e2c122000c";

} // namespace

int main() {
  using namespace ncs;
  TestRunner tests;
  core::application::SessionManager sessions;
  core::application::VerificationCodeService codes(true);
  core::application::InMemoryUserAccountRepository accounts;
  core::application::InMemoryChargingRepository charging;
  core::application::InMemoryAdminRepository adminRepository(
      charging, accounts, true);
  core::application::UserIdentityService identity(accounts, sessions, codes);
  core::application::BoundedExecutor blockingExecutor(2, 32);
  core::application::IdempotencyService idempotency;
  core::application::BusinessNumbers numbers;
  core::application::WalletService walletService(charging, accounts, numbers);
  core::application::ChargeFlowService flowService(charging, accounts, accounts,
                                                   numbers, 3600);
  core::application::AdminAuthService adminAuth(adminRepository, sessions);
  core::application::AdminUserService adminUsers(adminRepository, flowService,
                                                 sessions);
  core::application::AdminStationService adminStations(
      adminRepository, charging, flowService, numbers);
  core::application::AdminOpsService adminOps(adminRepository, charging,
                                              flowService, numbers);

  server::ServerApp app;
  server::controller::ApiRoutes api(app);
  server::controller::UserIdentityRoutes identityRoutes(
      api, identity, sessions, blockingExecutor, true);
  server::controller::WalletRoutes walletRoutes(api, walletService, sessions,
                                                blockingExecutor, idempotency);
  server::controller::FlowRoutes flowRoutes(api, flowService, sessions,
                                            blockingExecutor, idempotency);
  server::controller::AdminRoutes adminRoutes(
      api, adminAuth, adminUsers, adminStations, adminOps, sessions,
      blockingExecutor, idempotency);
  app.validate();

  // Seed a regular user with funds and a device-reserving flow.
  const std::string userCode = smsCode(app, "13800140001", "REGISTER");
  const auto registered = call(
      app, crow::HTTPMethod::POST, "/api/v1/user/auth/register",
      "{\"username\":\"managed_user\",\"phone\":\"13800140001\","
      "\"password\":\"managed-password\",\"smsCode\":\"" + userCode +
          "\",\"deviceId\":\"managed-terminal\"}");
  const std::string userToken =
      envelope(registered).value(QStringLiteral("data")).toObject()
          .value(QStringLiteral("accessToken"))
          .toString()
          .toStdString();
  tests.check(registered.code == 201, "managed user registered");

  tests.check(call(app, crow::HTTPMethod::GET, "/api/v1/admin/users")
                  .code == 401,
              "admin routes reject anonymous access");

  tests.check(call(app, crow::HTTPMethod::POST, "/api/v1/admin/auth/login",
                   R"({"username":"admin","password":"wrong-password","deviceId":"admin-desk"})")
                  .code == 401,
              "wrong admin password is rejected");

  const auto login = call(
      app, crow::HTTPMethod::POST, "/api/v1/admin/auth/login",
      R"({"username":"admin","password":"123456","deviceId":"admin-desk"})");
  const QJsonObject loginData = envelope(login).value("data").toObject();
  const std::string adminToken =
      loginData.value(QStringLiteral("accessToken")).toString().toStdString();
  tests.check(
      login.code == 200 && adminToken.size() >= 43 &&
          loginData.value("admin").toObject().value("roles").toArray().contains(
              QStringLiteral("OPERATOR")),
      "seeded admin logs in with operator role");

  const auto reauth = call(app, crow::HTTPMethod::POST,
                           "/api/v1/admin/auth/reauth",
                           R"({"password":"123456"})", adminToken);
  tests.check(reauth.code == 200, "reauthentication succeeds with password");

  const auto users = call(app, crow::HTTPMethod::GET, "/api/v1/admin/users?status=1",
                          {}, adminToken);
  tests.check(
      users.code == 200 &&
          envelope(users).value("data").toObject().value("total").toInt() == 1,
      "user list returns the managed account");
  tests.check(
      users.body.find("13800140001") == std::string::npos,
      "user list masks phone numbers");

  const std::int64_t userId = 1;
  const auto recharge = call(
      app, crow::HTTPMethod::POST, "/api/v1/user/wallet/recharges",
      R"({"amountCent":10000})", userToken, kKeyA);
  tests.check(recharge.code == 200, "managed user funded");

  const auto flowCreated = call(
      app, crow::HTTPMethod::POST, "/api/v1/user/flows",
      R"({"stationId":1,"chargerType":1,"preferredChargerId":null})", userToken,
      kKeyB);
  const QJsonObject flowData =
      envelope(flowCreated).value("data").toObject();
  const std::string flowNo =
      flowData.value(QStringLiteral("flowNo")).toString().toStdString();
  tests.check(
      flowCreated.code == 200 && flowData.value("status").toInt() == 20,
      "managed user holds a pending-quote flow");

  const auto disable = call(
      app, crow::HTTPMethod::POST, "/api/v1/admin/stations/1/disable",
      R"({"reason":"维护停用","version":1})", adminToken, kKeyG);
  tests.check(
      disable.code == 409 &&
          envelope(disable).value("code").toInt() == 15,
      "station with an active flow cannot be disabled");

  tests.check(
      call(app, crow::HTTPMethod::PUT,
           "/api/v1/admin/users/1/status",
           R"({"status":0,"reason":"人工审核冻结","version":1})", adminToken)
          .code == 400,
      "user freeze without an idempotency key is rejected");

  const auto freeze = call(
      app, crow::HTTPMethod::PUT, "/api/v1/admin/users/1/status",
      R"({"status":0,"reason":"人工审核冻结","version":1})", adminToken, kKeyC);
  tests.check(
      freeze.code == 200 &&
          envelope(freeze).value("data").toObject()
              .value("activeFlowPreserved") == true,
      "freeze preserves the active charging flow");
  tests.check(
      call(app, crow::HTTPMethod::GET, "/api/v1/user/me", {}, userToken)
              .code == 401,
      "freeze revokes the user sessions immediately");

  tests.check(
      call(app, crow::HTTPMethod::PUT, "/api/v1/admin/users/1/status",
           R"({"status":1,"reason":"审核通过","version":1})", adminToken, kKeyE)
              .code == 409,
      "stale user version conflicts on status update");

  // Unfreeze through the fresh version reported by the freeze response.
  const int unfrozenVersion =
      envelope(freeze).value("data").toObject().value("version").toInt();
  tests.check(
      call(app, crow::HTTPMethod::PUT, "/api/v1/admin/users/1/status",
           R"({"status":1,"reason":"审核通过","version":)" +
               std::to_string(unfrozenVersion) + "}",
           adminToken, kKeyD)
              .code == 200,
      "unfreeze succeeds with the fresh version");

  tests.check(
      call(app, crow::HTTPMethod::POST, "/api/v1/admin/chargers/batch",
           R"({"stationId":1,"chargers":[{"code":"ZGC-DC-01","chargerType":1,"powerWatt":60000}]})",
           adminToken, kKeyA)
              .code == 409,
      "duplicate charger code is rejected in batch creation");

  tests.check(
      call(app, crow::HTTPMethod::PUT, "/api/v1/admin/chargers/1/status",
           R"({"targetStatus":2,"reason":"人工巡检发现故障","version":1})",
           adminToken, kKeyE)
              .code == 409,
      "active device cannot change status directly");

  tests.check(
      call(app, crow::HTTPMethod::POST, "/api/v1/admin/tariffs",
           R"({"adcode":"110108","electricityPriceCentPerKwh":90,"servicePriceCentPerKwh":55,"effectiveFrom":1000,"reason":"重叠版本"})",
           adminToken, kKeyH)
          .code == 422,
      "overlapping tariff versions are rejected");
  tests.check(
      call(app, crow::HTTPMethod::POST, "/api/v1/admin/tariffs",
           R"({"adcode":"110101","electricityPriceCentPerKwh":88,"servicePriceCentPerKwh":52,"effectiveFrom":2000000000,"reason":"远期版本"})",
           adminToken, kKeyI)
          .code == 201,
      "non-overlapping tariff version is created");

  tests.check(
      call(app, crow::HTTPMethod::POST, "/api/v1/admin/price-adjustments",
           R"({"stationId":1,"chargerType":1,"source":"ML_APPROVED","adjustmentBp":300,"effectiveFrom":1,"effectiveTo":2,"reason":"非步长调整"})",
           adminToken)
              .code == 422,
      "service fee adjustment must follow the 500 bp step");

  // Force release on the pending-quote flow, then on charging flows refuse.
  tests.check(
      call(app, crow::HTTPMethod::POST,
           "/api/v1/admin/flows/" + flowNo + "/force-releases",
           R"({"confirm":true,"reason":"设备计划维护","nextChargerStatus":2,"flowVersion":99})",
           adminToken, kKeyE)
              .code == 409,
      "stale flow version conflicts on force release");
  const auto released = call(
      app, crow::HTTPMethod::POST,
      "/api/v1/admin/flows/" + flowNo + "/force-releases",
      R"({"confirm":true,"reason":"设备计划维护","nextChargerStatus":2,"flowVersion":1})",
      adminToken, kKeyD);
  tests.check(
      released.code == 200 &&
          envelope(released).value("data").toObject().value("status").toInt() == 70,
      "force release cancels the pending flow");

  // Restart a charging device: controlled settlement then a 2s command.
  const auto smsLogin = call(
      app, crow::HTTPMethod::POST, "/api/v1/user/auth/login/sms",
      "{\"phone\":\"13800140001\",\"smsCode\":\"" +
          smsCode(app, "13800140001", "LOGIN") +
          "\",\"deviceId\":\"managed-terminal2\"}");
  const std::string newToken =
      envelope(smsLogin).value("data").toObject()
          .value(QStringLiteral("accessToken"))
          .toString()
          .toStdString();
  tests.check(smsLogin.code == 200, "unfrozen user logs in again");
  const auto newFlow = call(
      app, crow::HTTPMethod::POST, "/api/v1/user/flows",
      R"({"stationId":1,"chargerType":1,"preferredChargerId":null})", newToken,
      kKeyF);
  tests.check(newFlow.code == 200, "user started a second flow");
  const QJsonObject newFlowData = envelope(newFlow).value("data").toObject();
  const std::string newFlowNo =
      newFlowData.value(QStringLiteral("flowNo")).toString().toStdString();
  const auto confirmed = call(
      app, crow::HTTPMethod::POST,
      "/api/v1/user/flows/" + newFlowNo + "/quote-confirmations",
      R"({"quoteNo":")" +
          newFlowData.value("quote").toObject()
              .value(QStringLiteral("quoteNo"))
              .toString()
              .toStdString() +
          R"(","flowVersion":1})",
      newToken, kKeyD);
  tests.check(confirmed.code == 200, "managed user confirmed the new quote");
  const auto started = call(
      app, crow::HTTPMethod::POST, "/api/v1/user/flows/" + newFlowNo + "/start",
      R"({"flowVersion":2})", newToken, kKeyA);
  tests.check(started.code == 200, "managed user started charging");

  const auto restart = call(
      app, crow::HTTPMethod::POST, "/api/v1/admin/chargers/2/restart-commands",
      R"({"confirm":true,"reason":"远程恢复测试"})", adminToken, kKeyC);
  tests.check(
      restart.code == 202 && envelope(restart)
                                    .value("data")
                                    .toObject()
                                    .value(QStringLiteral("status")) ==
                                QStringLiteral("PENDING") &&
          envelope(restart).value("data").toObject().value("chargerStatus").toInt() == 4,
      "restart command on charging device is accepted after controlled stop");
  bool settled = false;
  const auto adminFlows = call(
      app, crow::HTTPMethod::GET, "/api/v1/admin/flows?userId=1", {},
      adminToken);
  for (const auto &value :
       envelope(adminFlows).value("data").toObject().value("items").toArray()) {
    if (value.toObject().value("flowNo").toString().toStdString() == newFlowNo &&
        value.toObject().value("status").toInt() == 60)
      settled = true;
  }
  tests.check(settled, "restart controlled-settled the charging flow");

  const std::string commandNo =
      envelope(restart).value("data").toObject()
          .value(QStringLiteral("commandNo"))
          .toString()
          .toStdString();
  adminStations.completeDueCommands(std::chrono::system_clock::now() +
                                    std::chrono::seconds(3));
  const auto completedCommand = call(
      app, crow::HTTPMethod::GET,
      "/api/v1/admin/device-commands/" + commandNo, {}, adminToken);
  tests.check(
      completedCommand.code == 200 &&
          envelope(completedCommand).value("data").toObject().value("status") ==
              QStringLiteral("SUCCEEDED") &&
          charging.charger(2)->status == core::application::ChargerStatus::Idle,
      "device command completes and restores the charger to idle");

  tests.check(
      call(app, crow::HTTPMethod::GET, "/api/v1/admin/stats/revenue?bucket=day",
           {}, adminToken)
              .code == 200,
      "revenue statistics respond");
  tests.check(
      call(app, crow::HTTPMethod::GET,
           "/api/v1/admin/stats/charger-status?stationId=1", {}, adminToken)
              .code == 200,
      "charger status statistics respond");
  const auto auditLogs = call(app, crow::HTTPMethod::GET,
                              "/api/v1/admin/audit-logs?action=FORCE_RELEASE",
                              {}, adminToken);
  tests.check(
      auditLogs.code == 200 &&
          envelope(auditLogs).value("data").toObject().value("items").toArray().size() >= 1,
      "audit trail records the force release");
  tests.check(
      call(app, crow::HTTPMethod::GET, "/api/v1/admin/audit-logs", {}, userToken)
              .code == 401,
      "audit logs reject non-admin tokens");

  const auto backup = call(app, crow::HTTPMethod::POST, "/api/v1/admin/backups",
                           "{}", adminToken, kKeyC);
  tests.check(backup.code == 202, "backup orchestration returns 202");
  const std::string backupNo =
      envelope(backup).value("data").toObject()
          .value(QStringLiteral("backupNo"))
          .toString()
          .toStdString();
  const auto verified = call(
      app, crow::HTTPMethod::POST,
      "/api/v1/admin/backups/" + backupNo + "/verifications", "{}", adminToken,
      kKeyJ);
  tests.check(verified.code == 202, "backup verification returns 202");

  const auto trainTask = call(app, crow::HTTPMethod::POST, "/api/v1/admin/ml-tasks",
                              R"({"taskType":"TRAIN"})", adminToken, kKeyK);
  tests.check(trainTask.code == 202, "ml training task starts");
  const auto duplicateTask = call(app, crow::HTTPMethod::POST,
                                  "/api/v1/admin/ml-tasks",
                                  R"({"taskType":"TRAIN"})", adminToken, kKeyL);
  tests.check(
      duplicateTask.code == 202 &&
          envelope(duplicateTask).value("data").toObject().value("taskNo") ==
              envelope(trainTask).value("data").toObject().value("taskNo"),
      "duplicate ml task returns the running task");

  // UC-M-04: tasks must reach a terminal state instead of blocking the type
  // forever while no ML subprocess exists yet.
  core::application::MlTask agedTask;
  agedTask.taskNo = "MLAGED0000001";
  agedTask.taskType = "TRAIN";
  agedTask.status = "RUNNING";
  agedTask.createdAt =
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count() -
      (core::application::AdminOpsService::kTrainTimeoutSeconds + 60);
  adminRepository.addMlTask(agedTask);
  adminOps.completeTimedOutMlTasks(std::chrono::system_clock::now());
  const auto agedQuery = call(
      app, crow::HTTPMethod::GET, "/api/v1/admin/ml-tasks/MLAGED0000001", {},
      adminToken);
  tests.check(
      agedQuery.code == 200 &&
          envelope(agedQuery).value("data").toObject().value("status") ==
              QStringLiteral("TIMED_OUT"),
      "overdue ml task transitions to TIMED_OUT");
  tests.check(
      call(app, crow::HTTPMethod::GET, "/api/v1/admin/predictions", {}, adminToken)
              .code == 200,
      "predictions endpoint responds honestly empty");

  // Lockout: five consecutive failures lock the account for thirty seconds.
  for (int attempt = 0; attempt < 5; ++attempt) {
    call(app, crow::HTTPMethod::POST, "/api/v1/admin/auth/login",
         R"({"username":"admin","password":"wrong-password","deviceId":"admin-desk"})");
  }
  tests.check(
      call(app, crow::HTTPMethod::POST, "/api/v1/admin/auth/login",
           R"({"username":"admin","password":"123456","deviceId":"admin-desk"})")
              .code == 429,
      "five consecutive failures lock admin login");
  return tests.result();
}
