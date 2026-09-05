#include "server/controller/dashboard_routes.h"

#include "server/controller/api_response.h"
#include "server/controller/async_response.h"
#include "server/controller/request_validation.h"
#include "server/middleware/authorization.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace ncs::server::controller {
namespace {

using core::application::AuthContext;
using core::application::Role;
using core::application::TokenKind;
using core::domain::ErrorCode;

QJsonArray rolesJson(const std::vector<Role> &roles) {
  QJsonArray values;
  for (const auto role : roles)
    values.append(QString::fromStdString(core::application::roleName(role)));
  return values;
}

std::optional<AuthContext> requireDashboard(
    const crow::request &request, core::application::SessionManager &sessions,
    crow::response &failure) {
  const auto result = middleware::authorize(
      request, sessions, {TokenKind::Dashboard},
      {Role::Operator, Role::Owner, Role::Viewer},
      std::chrono::system_clock::now());
  if (!result.context) {
    failure = errorResponse(result.error, "dashboard authentication failed",
                            "大屏凭据无效或已过期");
    return std::nullopt;
  }
  return result.context;
}

} // namespace

QJsonObject dashboardSnapshotJson(
    const core::application::DashboardSnapshot &snapshot) {
  QJsonArray revenue;
  for (const auto &value : snapshot.revenue30d) {
    revenue.append(QJsonObject{
        {"bucketAt", static_cast<qint64>(value.bucketAt)},
        {"revenueCent", static_cast<qint64>(value.revenueCent)},
        {"energyMwh", static_cast<qint64>(value.energyMwh)},
        {"orderCount", value.orderCount}});
  }
  QJsonArray ranking;
  for (const auto &value : snapshot.stationRanking) {
    ranking.append(QJsonObject{
        {"stationId", static_cast<qint64>(value.stationId)},
        {"stationName", QString::fromStdString(value.stationName)},
        {"energyMwh", static_cast<qint64>(value.energyMwh)},
        {"revenueCent", static_cast<qint64>(value.revenueCent)},
        {"orderCount", value.orderCount}});
  }
  QJsonArray heatmap;
  for (const auto &value : snapshot.hourlyHeatmap) {
    heatmap.append(QJsonObject{{"weekday", value.weekday},
                               {"hour", value.hour},
                               {"energyMwh", static_cast<qint64>(value.energyMwh)},
                               {"orderCount", value.orderCount}});
  }
  QJsonArray predictions;
  for (const auto &value : snapshot.prediction24h) {
    predictions.append(QJsonObject{
        {"stationId", static_cast<qint64>(value.stationId)},
        {"horizonHour", value.horizonHour},
        {"modelVersionNo", QString::fromStdString(value.modelVersionNo)},
        {"generatedAt", static_cast<qint64>(value.generatedAt)},
        {"targetAt", static_cast<qint64>(value.targetAt)},
        {"predictedEnergyMwh", static_cast<qint64>(value.predictedEnergyMwh)},
        {"predictedFreeCount", value.predictedFreeCount},
        {"isPeak", value.isPeak},
        {"stale", value.stale}});
  }
  return QJsonObject{
      {"schemaVersion", snapshot.schemaVersion},
      {"dataVersion", static_cast<qint64>(snapshot.dataVersion)},
      {"generatedAt", static_cast<qint64>(snapshot.generatedAt)},
      {"stale", snapshot.stale},
      {"totalRevenueCent", static_cast<qint64>(snapshot.totalRevenueCent)},
      {"totalChargeCount", snapshot.totalChargeCount},
      {"registeredUserCount", snapshot.registeredUserCount},
      {"stationCount", snapshot.stationCount},
      {"chargerStatus", QJsonObject{{"idle", snapshot.idleCount},
                                     {"inUse", snapshot.inUseCount},
                                     {"fault", snapshot.faultCount},
                                     {"restarting", snapshot.restartingCount},
                                     {"disabled", snapshot.disabledCount}}},
      {"chargerTypeShare", QJsonObject{{"fast", snapshot.fastChargeCount},
                                        {"slow", snapshot.slowChargeCount}}},
      {"revenue30d", revenue},
      {"stationRanking", ranking},
      {"hourlyHeatmap", heatmap},
      {"prediction24h", predictions}};
}

DashboardRoutes::DashboardRoutes(
    ApiRoutes &routes, core::application::AdminAuthService &auth,
    core::application::DashboardService &dashboard,
    core::application::SessionManager &sessions,
    core::application::BoundedExecutor &executor, std::string snapshotPath,
    std::shared_ptr<core::application::EventHub> hub)
    : dashboard_(dashboard), writer_(std::move(snapshotPath)),
      hub_(std::move(hub)) {
  routes.route("/dashboard/auth/login").methods(crow::HTTPMethod::POST)(
      [&auth, &executor](const crow::request &request,
                         crow::response &response) {
        const auto parsed = parseJsonObject(
            request, {"username", "password", "deviceId"},
            {"username", "password", "deviceId"});
        if (!parsed.object ||
            !validStringField(*parsed.object, "username", 1, 64) ||
            !validStringField(*parsed.object, "password", 1, 128) ||
            !validStringField(*parsed.object, "deviceId", 1, 128)) {
          response = errorResponse(ErrorCode::Unauthorized,
                                   "invalid dashboard credentials",
                                   "账号或密码错误");
          response.end();
          return;
        }
        const std::string username =
            parsed.object->value("username").toString().toStdString();
        const std::string password =
            parsed.object->value("password").toString().toStdString();
        const std::string deviceId =
            parsed.object->value("deviceId").toString().toStdString();
        dispatchBlocking(request, response, executor,
                         [&auth, username, password, deviceId] {
          const auto result = auth.loginDashboard(
              username, password, deviceId, std::chrono::system_clock::now());
          if (!result.ok())
            return errorResponse(result.error, "dashboard login failed",
                                 result.error == ErrorCode::RateLimited
                                     ? "登录尝试过多，请稍后再试"
                                     : "账号或密码错误");
          const auto &value = *result.value;
          return successResponse(QJsonObject{
              {"accessToken", QString::fromStdString(value.session.accessToken)},
              {"expiresAt", static_cast<qint64>(
                  std::chrono::duration_cast<std::chrono::seconds>(
                      value.session.context.expiresAt.time_since_epoch()).count())},
              {"sessionId", static_cast<qint64>(value.session.context.sessionId)},
              {"admin", QJsonObject{{"id", static_cast<qint64>(value.admin.id)},
                                     {"username", QString::fromStdString(value.admin.username)},
                                     {"roles", rolesJson(value.admin.roles)}}}});
        });
      });

  routes.route("/dashboard/auth/logout").methods(crow::HTTPMethod::POST)(
      [&sessions](const crow::request &request, crow::response &response) {
        const auto bearer = core::application::SessionManager::parseBearer(
            request.get_header_value("Authorization"));
        if (!bearer) {
          response = errorResponse(ErrorCode::Unauthorized,
                                   "dashboard authentication failed",
                                   "大屏凭据无效或已过期");
          response.end();
          return;
        }
        const auto context = sessions.authenticate(
            *bearer, std::chrono::system_clock::now());
        // Unknown, expired, or already-revoked well-formed tokens are already
        // logged out. Return success so retries are idempotent without
        // disclosing whether a token ever existed.
        if (!context) {
          response = successResponse();
          response.end();
          return;
        }
        if (context->tokenKind != TokenKind::Dashboard) {
          response = errorResponse(ErrorCode::Forbidden,
                                   "Dashboard token required",
                                   "该凭据不能退出大屏会话");
          response.end();
          return;
        }
        sessions.revokeForPrincipal(context->principalId, context->sessionId);
        response = successResponse();
        response.end();
      });

  routes.route("/dashboard/summary").methods(crow::HTTPMethod::GET)(
      [&dashboard, &sessions, &executor](const crow::request &request,
                                         crow::response &response) {
        crow::response failure;
        if (!requireDashboard(request, sessions, failure)) {
          response = std::move(failure);
          response.end();
          return;
        }
        dispatchBlocking(request, response, executor, [&dashboard] {
          auto snapshot = dashboard.current();
          if (!snapshot) {
            const auto generated =
                dashboard.refresh(std::chrono::system_clock::now());
            if (!generated.ok())
              return errorResponse(ErrorCode::ExternalServiceUnavailable,
                                   "dashboard snapshot unavailable",
                                   "大屏数据暂时不可用");
            snapshot = generated.value;
          }
          return successResponse(dashboardSnapshotJson(*snapshot));
        });
      });
}

bool DashboardRoutes::refreshAndExport(
    const std::chrono::system_clock::time_point now) {
  const auto refreshed = dashboard_.refresh(now);
  if (!refreshed.ok())
    return false;
  const std::string json =
      QJsonDocument(dashboardSnapshotJson(*refreshed.value))
          .toJson(QJsonDocument::Compact)
          .toStdString();
  if (!writer_.write(json)) {
    dashboard_.markStale();
    return false;
  }
  if (hub_) {
    hub_->publish(
        "dashboard.refresh",
        "{\"dataVersion\":" +
            std::to_string(refreshed.value->dataVersion) + "}",
        core::application::EventScope{std::nullopt, std::nullopt, false, true},
        refreshed.value->generatedAt);
  }
  return true;
}

} // namespace ncs::server::controller
