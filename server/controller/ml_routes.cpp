#include "server/controller/ml_routes.h"

#include "server/controller/api_response.h"
#include "server/controller/async_response.h"
#include "server/controller/idempotent_response.h"
#include "server/controller/request_validation.h"
#include "server/middleware/authorization.h"

#include <QJsonArray>
#include <QJsonObject>

#include <cmath>

namespace ncs::server::controller {
namespace {

using core::application::AuthContext;
using core::application::MlCapability;
using core::application::TokenKind;
using core::domain::ErrorCode;

bool loopback(const std::string_view address) {
  return address == "::1" || address.rfind("127.", 0) == 0;
}

std::optional<AuthContext> requireMl(
    const crow::request &request, core::application::SessionManager &sessions,
    crow::response &failure) {
  if (!loopback(request.remote_ip_address)) {
    failure = errorResponse(ErrorCode::Forbidden, "loopback access required",
                            "该接口仅允许本机任务访问");
    return std::nullopt;
  }
  const auto result = middleware::authorize(
      request, sessions, {TokenKind::MlTask},
      {core::application::Role::MlWorker},
      std::chrono::system_clock::now());
  if (!result.context) {
    failure = errorResponse(result.error, "ML task authentication failed",
                            "ML 任务凭据无效或已过期");
    return std::nullopt;
  }
  return result.context;
}

bool finiteNumber(const QJsonObject &object, const char *field,
                  const double minimum, const double maximum) {
  const auto value = object.value(field);
  const double number = value.toDouble(std::numeric_limits<double>::quiet_NaN());
  return value.isDouble() && std::isfinite(number) && number >= minimum &&
         number <= maximum;
}

QJsonObject taskJson(const core::application::MlTask &task) {
  QJsonArray horizons;
  for (const int value : task.horizonHours)
    horizons.append(value);
  QJsonObject result{{"taskNo", QString::fromStdString(task.taskNo)},
                     {"taskType", QString::fromStdString(task.taskType)},
                     {"status", QString::fromStdString(task.status)},
                     {"horizonHours", horizons},
                     {"modelVersion", QString::fromStdString(task.modelVersion)},
                     {"createdAt", static_cast<qint64>(task.createdAt)},
                     {"metricsSummary", QString::fromStdString(task.metricsSummary)},
                     {"errorSummary", QString::fromStdString(task.errorSummary)}};
  if (task.finishedAt)
    result["finishedAt"] = static_cast<qint64>(*task.finishedAt);
  else
    result["finishedAt"] = QJsonValue(QJsonValue::Null);
  return result;
}

} // namespace

MlRoutes::MlRoutes(ApiRoutes &routes, core::application::MlService &ml,
                   core::application::SessionManager &sessions,
                   core::application::BoundedExecutor &executor,
                   core::application::IdempotencyService &idempotency) {
  routes.route("/internal/ml/features/hourly").methods(crow::HTTPMethod::GET)(
      [&ml, &sessions, &executor](const crow::request &request,
                                  crow::response &response) {
        crow::response failure;
        const auto auth = requireMl(request, sessions, failure);
        if (!auth) {
          response = std::move(failure);
          response.end();
          return;
        }
        const auto parameters = parseQueryParameters(
            request, {"taskNo", "fromAt", "toAt", "stationId", "cursor", "limit"});
        const auto fromAt = parameters
            ? parseIntegerParameter(*parameters, "fromAt", 0, 4102444800LL)
            : std::optional<std::optional<std::int64_t>>{};
        const auto toAt = parameters
            ? parseIntegerParameter(*parameters, "toAt", 1, 4102444800LL)
            : std::optional<std::optional<std::int64_t>>{};
        const auto stationId = parameters
            ? parseIntegerParameter(*parameters, "stationId", 1,
                                    9007199254740991LL)
            : std::optional<std::optional<std::int64_t>>{};
        const auto limitValue = parameters
            ? parseIntegerParameter(*parameters, "limit", 1, 5000)
            : std::optional<std::optional<std::int64_t>>{};
        if (!parameters || !fromAt || !*fromAt || !toAt || !*toAt ||
            !stationId || !limitValue || !*limitValue ||
            !parameters->count("taskNo")) {
          response = errorResponse(ErrorCode::ValidationFailed,
                                   "hourly feature parameters are invalid",
                                   "特征查询参数不符合要求");
          response.end();
          return;
        }
        const std::string taskNo = parameters->at("taskNo");
        if (!ml.authorize(*auth, taskNo, MlCapability::ReadFeatures)) {
          response = errorResponse(ErrorCode::Forbidden, "ML scope denied",
                                   "ML 任务作用域不匹配");
          response.end();
          return;
        }
        const std::optional<std::int64_t> station =
            *stationId ? std::optional<std::int64_t>(**stationId) : std::nullopt;
        const std::string cursor = parameters->count("cursor")
                                       ? parameters->at("cursor") : "";
        dispatchBlocking(request, response, executor,
                         [&ml, taskNo, from = **fromAt, to = **toAt, station,
                          cursor, limit = static_cast<int>(**limitValue)] {
          const auto result = ml.features(taskNo, from, to, station, cursor,
                                          limit, std::chrono::system_clock::now());
          if (!result.ok())
            return errorResponse(result.error, "hourly features unavailable",
                                 "特征数据暂时不可用");
          QJsonArray items;
          for (const auto &value : result.value->items) {
            items.append(QJsonObject{
                {"stationId", static_cast<qint64>(value.stationId)},
                {"stationName", QString::fromStdString(value.stationName)},
                {"bucketAt", static_cast<qint64>(value.bucketAt)},
                {"energyMwh", static_cast<qint64>(value.energyMwh)},
                {"orderCount", value.orderCount},
                {"fastOrderCount", value.fastOrderCount},
                {"slowOrderCount", value.slowOrderCount},
                {"operationalChargerCount", value.operationalChargerCount},
                {"busyDeviceSeconds", static_cast<qint64>(value.busyDeviceSeconds)}});
          }
          return successResponse(QJsonObject{
              {"items", items},
              {"nextCursor", result.value->nextCursor.empty()
                                     ? QJsonValue(QJsonValue::Null)
                                     : QJsonValue(QString::fromStdString(
                                           result.value->nextCursor))}});
        });
      });

  routes.route("/internal/ml/model-versions").methods(crow::HTTPMethod::POST)(
      [&ml, &sessions, &executor](const crow::request &request,
                                  crow::response &response) {
        crow::response failure;
        const auto auth = requireMl(request, sessions, failure);
        const auto parsed = parseJsonObject(
            request,
            {"taskNo", "algorithm", "featureSchemaVersion", "randomSeed",
             "trainFromAt", "trainToAt", "mae", "rmse", "mape", "wape",
             "baselineMae", "baselineRmse", "excludedSampleCount",
             "artifactChecksum"},
            {"taskNo", "algorithm", "featureSchemaVersion", "randomSeed",
             "trainFromAt", "trainToAt", "mae", "rmse", "mape", "wape",
             "baselineMae", "baselineRmse", "excludedSampleCount",
             "artifactChecksum"});
        if (!auth) {
          response = std::move(failure);
          response.end();
          return;
        }
        if (!parsed.object || !validStringField(*parsed.object, "taskNo", 3, 64) ||
            !validStringField(*parsed.object, "algorithm", 1, 64) ||
            !validStringField(*parsed.object, "artifactChecksum", 64, 64) ||
            !validIntegerField(*parsed.object, "featureSchemaVersion", 1, 1000) ||
            !validIntegerField(*parsed.object, "randomSeed", 0, 2147483647) ||
            !validIntegerField(*parsed.object, "trainFromAt", 0, 4102444800LL) ||
            !validIntegerField(*parsed.object, "trainToAt", 1, 4102444800LL) ||
            !validIntegerField(*parsed.object, "excludedSampleCount", 0, 100000000) ||
            !finiteNumber(*parsed.object, "mae", 0, 1e15) ||
            !finiteNumber(*parsed.object, "rmse", 0, 1e15) ||
            !finiteNumber(*parsed.object, "mape", 0, 1e9) ||
            !finiteNumber(*parsed.object, "wape", 0, 1e9) ||
            !finiteNumber(*parsed.object, "baselineMae", 0, 1e15) ||
            !finiteNumber(*parsed.object, "baselineRmse", 0, 1e15)) {
          response = errorResponse(ErrorCode::ValidationFailed,
                                   "model version payload is invalid",
                                   "模型登记内容不符合要求");
          response.end();
          return;
        }
        const std::string taskNo = parsed.object->value("taskNo").toString().toStdString();
        if (!ml.authorize(*auth, taskNo, MlCapability::RegisterModel)) {
          response = errorResponse(ErrorCode::Forbidden, "ML scope denied",
                                   "ML 任务作用域不匹配");
          response.end();
          return;
        }
        core::application::ModelVersion version;
        version.algorithm = parsed.object->value("algorithm").toString().toStdString();
        version.featureSchemaVersion = parsed.object->value("featureSchemaVersion").toInt();
        version.randomSeed = parsed.object->value("randomSeed").toInteger();
        version.trainFromAt = parsed.object->value("trainFromAt").toInteger();
        version.trainToAt = parsed.object->value("trainToAt").toInteger();
        version.mae = parsed.object->value("mae").toDouble();
        version.rmse = parsed.object->value("rmse").toDouble();
        version.mape = parsed.object->value("mape").toDouble();
        version.wape = parsed.object->value("wape").toDouble();
        version.baselineMae = parsed.object->value("baselineMae").toDouble();
        version.baselineRmse = parsed.object->value("baselineRmse").toDouble();
        version.excludedSampleCount = parsed.object->value("excludedSampleCount").toInt();
        version.artifactChecksum = parsed.object->value("artifactChecksum").toString().toStdString();
        dispatchBlocking(request, response, executor,
                         [&ml, taskNo, version = std::move(version)]() mutable {
          const auto result = ml.registerModel(taskNo, std::move(version),
                                               std::chrono::system_clock::now());
          if (!result.ok())
            return errorResponse(result.error, "model registration failed",
                                 "模型登记失败");
          return successResponse(QJsonObject{
              {"modelVersionNo", QString::fromStdString(result.value->versionNo)},
              {"qualified", result.value->qualified}}, 201);
        });
      });

  routes.route("/internal/ml/predictions/batch").methods(crow::HTTPMethod::POST)(
      [&ml, &sessions, &executor, &idempotency](const crow::request &request,
                                                crow::response &response) {
        crow::response failure;
        const auto auth = requireMl(request, sessions, failure);
        const auto parsed = parseJsonObject(request, {"taskNo", "modelVersionNo", "items"},
                                            {"taskNo", "modelVersionNo", "items"},
                                            8 * 1024 * 1024);
        if (!auth) {
          response = std::move(failure);
          response.end();
          return;
        }
        if (!parsed.object || !validStringField(*parsed.object, "taskNo", 3, 64) ||
            !validStringField(*parsed.object, "modelVersionNo", 3, 64) ||
            !parsed.object->value("items").isArray()) {
          response = errorResponse(ErrorCode::ValidationFailed,
                                   "prediction batch payload is invalid",
                                   "预测批次内容不符合要求");
          response.end();
          return;
        }
        const std::string taskNo = parsed.object->value("taskNo").toString().toStdString();
        if (!ml.authorize(*auth, taskNo, MlCapability::WritePredictions)) {
          response = errorResponse(ErrorCode::Forbidden, "ML scope denied",
                                   "ML 任务作用域不匹配");
          response.end();
          return;
        }
        const std::string model = parsed.object->value("modelVersionNo").toString().toStdString();
        std::vector<core::application::LoadPrediction> values;
        const auto items = parsed.object->value("items").toArray();
        if (items.isEmpty() || items.size() > 5000) {
          response = errorResponse(ErrorCode::ValidationFailed,
                                   "prediction batch size is invalid",
                                   "预测批次大小不符合要求");
          response.end();
          return;
        }
        for (const auto itemValue : items) {
          const auto item = itemValue.toObject();
          if (!itemValue.isObject() ||
              !hasOnlyFields(item, {"stationId", "generatedAt", "targetAt",
                                    "horizonHour", "predictedEnergyMwh",
                                    "predictedFreeCount", "isPeak"}) ||
              !validIntegerField(item, "stationId", 1, 9007199254740991LL) ||
              !validIntegerField(item, "generatedAt", 0, 4102444800LL) ||
              !validIntegerField(item, "targetAt", 1, 4102444800LL) ||
              !validIntegerField(item, "horizonHour", 1, 24) ||
              !validIntegerField(item, "predictedEnergyMwh", 0, 1000000000000LL) ||
              !validIntegerField(item, "predictedFreeCount", 0, 10000) ||
              !item.value("isPeak").isBool()) {
            response = errorResponse(ErrorCode::ValidationFailed,
                                     "prediction item is invalid",
                                     "预测条目不符合要求");
            response.end();
            return;
          }
          values.push_back(core::application::LoadPrediction{
              item.value("stationId").toInteger(), item.value("horizonHour").toInt(),
              model, item.value("generatedAt").toInteger(),
              item.value("targetAt").toInteger(),
              item.value("predictedEnergyMwh").toInteger(),
              item.value("predictedFreeCount").toInt(),
              item.value("isPeak").toBool(), false});
        }
        dispatchIdempotentBlocking(
            request, response, executor, idempotency,
            "internal-ml-predictions:" + taskNo, false,
            [&ml, taskNo, values = std::move(values)](auto &) {
              const auto result = ml.writePredictions(
                  taskNo, values, std::chrono::system_clock::now());
              if (!result.ok())
                return errorResponse(result.error, "prediction batch failed",
                                     "预测回写失败");
              return successResponse(
                  QJsonObject{{"acceptedCount", static_cast<qint64>(*result.value)}});
            });
      });

  routes.route("/internal/ml/tasks/<string>/completion")
      .methods(crow::HTTPMethod::POST)(
          [&ml, &sessions, &executor](const crow::request &request,
                                      crow::response &response,
                                      std::string taskNo) {
            crow::response failure;
            const auto auth = requireMl(request, sessions, failure);
            const auto parsed = parseJsonObject(
                request, {"status", "modelVersionNo", "metricsSummary", "errorSummary"},
                {"status"});
            if (!auth) {
              response = std::move(failure);
              response.end();
              return;
            }
            if (!ml.authorize(*auth, taskNo, MlCapability::Complete) ||
                !parsed.object || !validStringField(*parsed.object, "status", 6, 9) ||
                (parsed.object->contains("modelVersionNo") &&
                 !validStringField(*parsed.object, "modelVersionNo", 0, 64)) ||
                (parsed.object->contains("metricsSummary") &&
                 !validStringField(*parsed.object, "metricsSummary", 0, 500)) ||
                (parsed.object->contains("errorSummary") &&
                 !validStringField(*parsed.object, "errorSummary", 0, 500))) {
              response = errorResponse(ErrorCode::ValidationFailed,
                                       "task completion payload is invalid",
                                       "任务完成内容不符合要求");
              response.end();
              return;
            }
            const std::string status = parsed.object->value("status").toString().toStdString();
            if (status != "SUCCEEDED" && status != "FAILED") {
              response = errorResponse(ErrorCode::ValidationFailed,
                                       "task completion status is invalid",
                                       "任务完成状态不符合要求");
              response.end();
              return;
            }
            const std::int64_t sessionId = auth->sessionId;
            const auto receivedAt = std::chrono::system_clock::now();
            dispatchBlocking(request, response, executor,
                             [&ml, &sessions, taskNo, status, sessionId,
                              receivedAt,
                              model = parsed.object->value("modelVersionNo").toString().toStdString(),
                              metrics = parsed.object->value("metricsSummary").toString().toStdString(),
                              error = parsed.object->value("errorSummary").toString().toStdString()] {
              const auto result = ml.complete(taskNo, status == "SUCCEEDED", model,
                                              metrics, error, receivedAt);
              if (!result.ok())
                return errorResponse(result.error, "ML task completion failed",
                                     "任务完成状态写入失败");
              sessions.revoke(sessionId);
              return successResponse(taskJson(*result.value));
            });
          });
}

} // namespace ncs::server::controller
