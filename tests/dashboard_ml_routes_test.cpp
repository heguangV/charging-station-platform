#include "core/application/admin_auth_service.h"
#include "core/application/analytics_service.h"
#include "core/application/bounded_executor.h"
#include "core/application/business_numbers.h"
#include "core/application/idempotency_service.h"
#include "core/application/security_crypto.h"
#include "infrastructure/files/model_artifact_store.h"
#include "infrastructure/sqlite/sqlite_repository.h"
#include "server/controller/api_routes.h"
#include "server/controller/dashboard_routes.h"
#include "server/controller/ml_routes.h"
#include "server/server_app.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QTemporaryDir>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

class Tests final {
public:
  void check(bool condition, const char *message) {
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
  return QJsonDocument::fromJson(QByteArray::fromStdString(response.body)).object();
}

crow::response call(ncs::server::ServerApp &app, crow::HTTPMethod method,
                    std::string url, std::string body = {},
                    const std::string &token = {},
                    const std::string &key = {},
                    const std::string &remote = "127.0.0.1") {
  const auto question = url.find('?');
  crow::request request(method, url, url.substr(0, question),
                        crow::query_string(url), {}, body, 1, 1, true, false,
                        false);
  request.remote_ip_address = remote;
  request.add_header("Content-Type", "application/json; charset=utf-8");
  if (!token.empty())
    request.add_header("Authorization", "Bearer " + token);
  if (!key.empty())
    request.add_header("Idempotency-Key", key);
  crow::response response;
  app.handle_full(request, response);
  return response;
}

std::int64_t nowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

int main() {
  using namespace ncs;
  Tests tests;
  QTemporaryDir temporary;
  tests.check(temporary.isValid(), "temporary directory is available");
  infrastructure::sqlite::SqliteRepository repository(
      (temporary.path() + "/analytics.db").toStdString());
  repository.ensureDevelopmentAdmin(true);
  core::application::SessionManager sessions;
  core::application::AdminAuthService auth(repository, sessions);
  core::application::BusinessNumbers numbers(&repository);
  core::application::DashboardService dashboard(repository, repository,
                                                 repository, repository);
  core::application::MlService ml(repository, repository, repository, numbers);
  core::application::BoundedExecutor executor(2, 32);
  core::application::IdempotencyService idempotency(&repository);
  server::ServerApp app;
  server::controller::ApiRoutes api(app);
  server::controller::DashboardRoutes dashboardRoutes(
      api, auth, dashboard, sessions, executor,
      (temporary.path() + "/dashboard.json").toStdString());
  server::controller::MlRoutes mlRoutes(api, ml, sessions, executor, idempotency);
  app.validate();

  tests.check(call(app, crow::HTTPMethod::GET, "/api/v1/dashboard/summary").code == 401,
              "anonymous dashboard reads are rejected");
  const auto login = call(
      app, crow::HTTPMethod::POST, "/api/v1/dashboard/auth/login",
      R"({"username":"admin","password":"123456","deviceId":"dashboard-test"})");
  const auto loginData = envelope(login).value("data").toObject();
  const std::string dashboardToken = loginData.value("accessToken").toString().toStdString();
  tests.check(login.code == 200 && dashboardToken.size() >= 43,
              "operator receives an isolated Dashboard token");
  const auto summary = call(app, crow::HTTPMethod::GET,
                            "/api/v1/dashboard/summary", {}, dashboardToken);
  const auto summaryData = envelope(summary).value("data").toObject();
  tests.check(summary.code == 200 && summaryData.value("schemaVersion").toInt() == 1 &&
                  summaryData.value("hourlyHeatmap").toArray().size() == 168 &&
                  summaryData.value("revenue30d").toArray().size() == 30,
              "dashboard returns a complete zero-safe snapshot");
  auto viewerToken = sessions.issue(
      "admin:999", "viewer-dashboard", core::application::TokenKind::Dashboard,
      {core::application::Role::Viewer}, std::chrono::system_clock::now(),
      std::chrono::hours(8));
  tests.check(viewerToken &&
                  call(app, crow::HTTPMethod::GET, "/api/v1/dashboard/summary", {},
                       viewerToken->accessToken).code == 200 &&
                  !core::application::SessionManager::allowsPath(
                      core::application::TokenKind::Dashboard,
                      "/api/v1/admin/users"),
              "decision viewer token is read-only and Dashboard-scoped");
  tests.check(dashboardRoutes.refreshAndExport(std::chrono::system_clock::now()) &&
                  QFileInfo(temporary.path() + "/dashboard.json").size() > 0,
              "dashboard snapshot is atomically exported");
  tests.check(call(app, crow::HTTPMethod::POST,
                   "/api/v1/dashboard/auth/logout", {}, dashboardToken).code == 200 &&
                  call(app, crow::HTTPMethod::POST,
                       "/api/v1/dashboard/auth/logout", {}, dashboardToken).code == 200,
              "Dashboard logout is idempotent and revokes the session");

  infrastructure::files::FileModelArtifactStore artifactStore(
      (temporary.path() + "/models/load_rf.pkl").toStdString());
  QFile staged(QString::fromStdString(artifactStore.stagingPath("MLARTIFACT")));
  QDir().mkpath(QFileInfo(staged).absolutePath());
  tests.check(staged.open(QIODevice::WriteOnly) && staged.write("model") == 5,
              "task-owned model staging file is writable");
  staged.close();
  tests.check(artifactStore.verify(
                  "MLARTIFACT", core::application::sha256Hex("model")) &&
                  artifactStore.finalize("MLARTIFACT", true, "MODEL0001") &&
                  QFileInfo(QString::fromStdString(artifactStore.activePath())).size() == 5,
              "verified model artifact is atomically activated");
  {
    namespace fs = std::filesystem;
    const fs::path retained = artifactStore.artifactPath("MODEL0001");
    const fs::path stale = retained.parent_path() / "MODEL0000.pkl";
    {
      std::ofstream file(stale.string(), std::ios::binary);
      file << "old";
    }
    std::error_code error;
    const auto aged = fs::file_time_type::clock::now() - std::chrono::hours(24 * 91);
    fs::last_write_time(retained, aged, error);
    fs::last_write_time(stale, aged, error);
    artifactStore.cleanupExpired(retained.string());
    tests.check(fs::exists(retained) && !fs::exists(stale),
                "cleanup keeps the newest qualified model's artifact and "
                "removes only expired other artifacts");
  }

  const std::int64_t now = nowSeconds();
  core::application::MlTask trainTask;
  trainTask.taskNo = "MLTRAIN000001";
  trainTask.taskType = "TRAIN";
  trainTask.status = "RUNNING";
  trainTask.createdAt = now;
  repository.addMlTask(trainTask);
  auto trainToken = sessions.issue("ml:" + trainTask.taskNo, "worker-train",
                                   core::application::TokenKind::MlTask,
                                   {core::application::Role::MlWorker,
                                    core::application::Role::MlTrainer},
                                   std::chrono::system_clock::now(),
                                   std::chrono::minutes(10));
  tests.check(trainToken.has_value(), "scoped training token is issued");
  const std::string featureUrl =
      "/api/v1/internal/ml/features/hourly?taskNo=" + trainTask.taskNo +
      "&fromAt=" + std::to_string(now - 24 * 3600) + "&toAt=" +
      std::to_string(now) + "&limit=20";
  const auto features = call(app, crow::HTTPMethod::GET, featureUrl, {},
                             trainToken->accessToken);
  tests.check(features.code == 200 &&
                  envelope(features).value("data").toObject()
                      .value("items").toArray().size() == 20,
              "hourly feature cursor endpoint fills zero-order hours");
  tests.check(call(app, crow::HTTPMethod::GET, featureUrl, {},
                   trainToken->accessToken, {}, "10.0.0.2").code == 403,
              "ML internal API rejects non-loopback clients");
  tests.check(call(app, crow::HTTPMethod::POST,
                   "/api/v1/internal/ml/predictions/batch",
                   "{\"taskNo\":\"" + trainTask.taskNo +
                       "\",\"modelVersionNo\":\"BASELINE\",\"items\":[]}",
                   trainToken->accessToken,
                   "7cb640c6-6995-4be5-9161-f0e2c1220010").code == 403,
              "training token cannot use prediction-write scope");

  const std::string checksum(64, 'a');
  const auto model = call(
      app, crow::HTTPMethod::POST, "/api/v1/internal/ml/model-versions",
      "{\"taskNo\":\"" + trainTask.taskNo +
          "\",\"algorithm\":\"RandomForestRegressor\","
          "\"featureSchemaVersion\":1,\"randomSeed\":20260901,"
          "\"trainFromAt\":" + std::to_string(now - 24 * 3600) +
          ",\"trainToAt\":" + std::to_string(now - 1) +
          ",\"mae\":1.0,\"rmse\":2.0,\"mape\":3.0,\"wape\":4.0,"
          "\"baselineMae\":2.0,\"baselineRmse\":3.0,"
          "\"excludedSampleCount\":5,\"artifactChecksum\":\"" + checksum + "\"}",
      trainToken->accessToken);
  const auto modelData = envelope(model).value("data").toObject();
  const std::string modelVersion = modelData.value("modelVersionNo").toString().toStdString();
  tests.check(model.code == 201 && modelData.value("qualified").toBool() &&
                  !modelVersion.empty(),
              "qualified fixed-seed model metadata is persisted");
  const auto trainCompletion = call(
      app, crow::HTTPMethod::POST,
      "/api/v1/internal/ml/tasks/" + trainTask.taskNo + "/completion",
      "{\"status\":\"SUCCEEDED\",\"modelVersionNo\":\"" + modelVersion +
          "\",\"metricsSummary\":\"ok\",\"errorSummary\":\"\"}",
      trainToken->accessToken);
  tests.check(trainCompletion.code == 200 &&
                  !sessions.authenticate(trainToken->accessToken,
                                         std::chrono::system_clock::now()),
              "task completion persists state and revokes its one-task token");

  core::application::MlTask predictionTask;
  predictionTask.taskNo = "MLPREDICT0001";
  predictionTask.taskType = "PREDICT";
  predictionTask.status = "RUNNING";
  predictionTask.horizonHours = {1, 6, 24};
  predictionTask.createdAt = now;
  repository.addMlTask(predictionTask);
  auto predictionToken = sessions.issue(
      "ml:" + predictionTask.taskNo, "worker-predict",
      core::application::TokenKind::MlTask,
      {core::application::Role::MlWorker,
       core::application::Role::MlPredictor}, std::chrono::system_clock::now(),
      std::chrono::minutes(2));
  const std::int64_t generatedAt = now;
  const auto batch = call(
      app, crow::HTTPMethod::POST, "/api/v1/internal/ml/predictions/batch",
      "{\"taskNo\":\"" + predictionTask.taskNo +
          "\",\"modelVersionNo\":\"BASELINE\",\"items\":[{"
          "\"stationId\":1,\"generatedAt\":" + std::to_string(generatedAt) +
          ",\"targetAt\":" + std::to_string(generatedAt + 3600) +
          ",\"horizonHour\":1,\"predictedEnergyMwh\":1234,"
          "\"predictedFreeCount\":2,\"isPeak\":false}]}",
      predictionToken->accessToken,
      "6cb640c6-6995-4be5-9161-f0e2c1220010");
  tests.check(batch.code == 200 && repository.predictions(1, 1, now).size() == 1,
              "prediction batch is validated and transactionally upserted");
  tests.check(ml.complete(predictionTask.taskNo, true, "BASELINE", {}, {},
                          std::chrono::system_clock::time_point(
                              std::chrono::seconds(now + 1)))
                  .ok(),
              "prediction task reaches a terminal state before the next run");

  core::application::MlTask boundaryTask;
  boundaryTask.taskNo = "MLBOUNDARY001";
  boundaryTask.taskType = "PREDICT";
  boundaryTask.status = "RUNNING";
  boundaryTask.createdAt = now;
  repository.addMlTask(boundaryTask);
  auto timedOutBoundary = boundaryTask;
  timedOutBoundary.status = "TIMED_OUT";
  timedOutBoundary.finishedAt = now + 121;
  tests.check(repository.tryFinishMlTask(timedOutBoundary, false),
              "timeout wins the first terminal-state CAS");
  const auto boundaryCompletion = ml.complete(
      boundaryTask.taskNo, false, {}, {}, "worker failure",
      std::chrono::system_clock::time_point(std::chrono::seconds(now + 119)));
  tests.check(boundaryCompletion.ok() &&
                  boundaryCompletion.value->status == "FAILED" &&
                  repository.mlTask(boundaryTask.taskNo)->status == "FAILED",
              "an on-time completion queued before the deadline can resolve a timeout race");
  tests.check(!repository.tryFinishMlTask(timedOutBoundary, false),
              "a completed ML task cannot be overwritten by another terminal writer");

  executor.shutdown();
  return tests.result();
}
