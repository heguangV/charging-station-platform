#include "server/controller/admin_routes.h"

#include "core/domain/error_code.h"
#include "server/controller/api_response.h"
#include "server/controller/async_response.h"
#include "server/controller/idempotent_response.h"
#include "server/controller/request_validation.h"
#include "server/controller/user_identity_dto.h"
#include "server/middleware/authorization.h"

#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>
#include <cctype>

namespace ncs::server::controller {
namespace {

using core::application::AuthContext;
using core::domain::ErrorCode;

std::optional<AuthContext> requireAdmin(const crow::request &request,
                                        core::application::SessionManager &sessions,
                                        crow::response &failure) {
  const auto result = middleware::authorize(
      request, sessions, {core::application::TokenKind::Administrator},
      {core::application::Role::Operator, core::application::Role::Owner},
      std::chrono::system_clock::now());
  if (!result.context) {
    failure = errorResponse(result.error, "authentication failed",
                            "管理员凭据无效或已过期");
    return std::nullopt;
  }
  return result.context;
}

std::optional<AuthContext> requireOwner(const crow::request &request,
                                        core::application::SessionManager &sessions,
                                        crow::response &failure) {
  const auto result = middleware::authorize(
      request, sessions, {core::application::TokenKind::Administrator},
      {core::application::Role::Owner}, std::chrono::system_clock::now());
  if (!result.context) {
    failure = errorResponse(result.error, "owner role required", "需要 OWNER 权限");
    return std::nullopt;
  }
  return result.context;
}

std::optional<std::int64_t> adminId(const AuthContext &auth) {
  return core::application::AdminAuthService::principalId(auth.principalId);
}

QJsonArray rolesJson(const std::vector<core::application::Role> &roles) {
  QJsonArray array;
  for (const auto role : roles)
    array.append(QString::fromStdString(
        core::application::roleName(role)));
  return array;
}

QJsonObject adminJson(const core::application::AdminAccount &admin) {
  return {
      {QStringLiteral("id"), QJsonValue(static_cast<qint64>(admin.id))},
      {QStringLiteral("username"), QString::fromStdString(admin.username)},
      {QStringLiteral("roles"), rolesJson(admin.roles)},
      {QStringLiteral("mustChangePassword"), admin.mustChangePassword},
  };
}

QJsonObject chargerJson(const core::application::Charger &charger) {
  return {
      {QStringLiteral("id"), QJsonValue(static_cast<qint64>(charger.id))},
      {QStringLiteral("stationId"), QJsonValue(static_cast<qint64>(charger.stationId))},
      {QStringLiteral("code"), QString::fromStdString(charger.code)},
      {QStringLiteral("chargerType"), charger.type == core::application::ChargerType::DcFast ? 1 : 0},
      {QStringLiteral("powerWatt"), QJsonValue(static_cast<qint64>(charger.powerWatt))},
      {QStringLiteral("connectorStandard"), QString::fromStdString(charger.connectorStandard)},
      {QStringLiteral("status"), static_cast<int>(charger.status)},
      {QStringLiteral("statusText"),
          QString::fromStdString(core::application::chargerStatusText(static_cast<int>(charger.status)))},
      {QStringLiteral("totalCount"), QJsonValue(static_cast<qint64>(charger.totalCount))},
      {QStringLiteral("totalMinutes"), QJsonValue(static_cast<qint64>(charger.totalMinutes))},
      {QStringLiteral("version"), QJsonValue(static_cast<qint64>(charger.version))},
  };
}

QJsonObject tariffJson(const core::application::RegionTariff &tariff) {
  return {
      {QStringLiteral("adcode"), QString::fromStdString(tariff.adcode)},
      {QStringLiteral("electricityPriceCentPerKwh"), tariff.electricityCentPerKwh},
      {QStringLiteral("servicePriceCentPerKwh"), tariff.serviceCentPerKwh},
      {QStringLiteral("effectiveFrom"), QJsonValue(static_cast<qint64>(tariff.effectiveFrom))},
      {QStringLiteral("effectiveTo"), QJsonValue(static_cast<qint64>(tariff.effectiveTo))},
  };
}

QJsonObject deviceCommandJson(const core::application::DeviceCommand &command) {
  QJsonObject data{
      {QStringLiteral("commandNo"), QString::fromStdString(command.commandNo)},
      {QStringLiteral("chargerId"), QJsonValue(static_cast<qint64>(command.chargerId))},
      {QStringLiteral("chargerCode"), QString::fromStdString(command.chargerCode)},
      {QStringLiteral("status"), QString::fromStdString(command.status)},
      {QStringLiteral("createdAt"), QJsonValue(static_cast<qint64>(command.createdAt))},
  };
  if (command.completedAt)
    data.insert(QStringLiteral("completedAt"), QJsonValue(static_cast<qint64>(*command.completedAt)));
  else
    data.insert(QStringLiteral("completedAt"), QJsonValue(QJsonValue::Null));
  if (!command.errorSummary.empty())
    data.insert(QStringLiteral("errorSummary"), QString::fromStdString(command.errorSummary));
  return data;
}

QJsonObject flowJson(const core::application::ChargingFlow &flow) {
  QJsonObject data{
      {QStringLiteral("flowNo"), QString::fromStdString(flow.flowNo)},
      {QStringLiteral("userId"), QJsonValue(static_cast<qint64>(flow.userId))},
      {QStringLiteral("stationId"), QJsonValue(static_cast<qint64>(flow.stationId))},
      {QStringLiteral("chargerType"), flow.chargerType == core::application::ChargerType::DcFast ? 1 : 0},
      {QStringLiteral("status"), flow.status},
      {QStringLiteral("statusText"),
          QString::fromStdString(core::application::flowStatusText(flow.status))},
      {QStringLiteral("version"), QJsonValue(static_cast<qint64>(flow.version))},
      {QStringLiteral("createdAt"), QJsonValue(static_cast<qint64>(flow.createdAt))},
  };
  if (flow.chargerId)
    data.insert(QStringLiteral("chargerId"), QJsonValue(static_cast<qint64>(*flow.chargerId)));
  else
    data.insert(QStringLiteral("chargerId"), QJsonValue(QJsonValue::Null));
  if (flow.chargerCode)
    data.insert(QStringLiteral("chargerCode"), QString::fromStdString(*flow.chargerCode));
  else
    data.insert(QStringLiteral("chargerCode"), QJsonValue(QJsonValue::Null));
  return data;
}

QJsonObject auditJson(const core::application::AuditEvent &event) {
  return {
      {QStringLiteral("actorId"), QString::fromStdString("admin:" + std::to_string(event.actorAdminId))},
      {QStringLiteral("action"), QString::fromStdString(event.action)},
      {QStringLiteral("targetType"), QString::fromStdString(event.targetType)},
      {QStringLiteral("targetId"), QString::fromStdString(event.targetId)},
      {QStringLiteral("reason"), QString::fromStdString(event.reason)},
      {QStringLiteral("at"), QJsonValue(static_cast<qint64>(event.at))},
  };
}

std::optional<Pagination> adminPagination(const crow::request &request,
                                          const std::unordered_set<std::string> &filters,
                                          const std::unordered_set<std::string> &sorts = {}) {
  return parsePagination(request, sorts, filters);
}

bool decimalDigits(const std::string_view value, const std::size_t size) {
  return value.size() == size &&
         std::all_of(value.begin(), value.end(), [](const unsigned char character) {
           return std::isdigit(character) != 0;
         });
}

} // namespace

AdminRoutes::AdminRoutes(
    ApiRoutes &routes, core::application::AdminAuthService &auth,
    core::application::AdminUserService &users,
    core::application::AdminStationService &stations,
    core::application::AdminOpsService &ops,
    core::application::SessionManager &sessions,
    core::application::BoundedExecutor &executor,
    core::application::IdempotencyService &idempotency) {
  routes.route("/admin/auth/login").methods(crow::HTTPMethod::POST)(
      [&auth, &executor](const crow::request &request, crow::response &response) {
        const auto parsed = parseJsonObject(request, {"username", "password", "deviceId"},
                                           {"username", "password", "deviceId"});
        if (!parsed.object || !parsed.object->value("username").isString() ||
            !parsed.object->value("password").isString() ||
            !parsed.object->value("deviceId").isString()) {
          response = errorResponse(ErrorCode::InvalidArgument, "login request is invalid",
                                   "登录请求内容不符合要求");
          response.end();
          return;
        }
        std::string username = parsed.object->value("username").toString().toStdString();
        std::string password = parsed.object->value("password").toString().toStdString();
        std::string deviceId = parsed.object->value("deviceId").toString().toStdString();
        dispatchBlocking(request, response, executor,
                         [&auth, username = std::move(username), password = std::move(password),
                          deviceId = std::move(deviceId)]() mutable {
                           const auto result = auth.login(
                               std::move(username), std::move(password), std::move(deviceId),
                               std::chrono::system_clock::now());
                           if (!result.ok())
                             return errorResponse(result.error, "", "");
                           return successResponse(QJsonObject{
                               {QStringLiteral("accessToken"),
                                   QString::fromStdString(result.value->session.accessToken)},
                               {QStringLiteral("expiresAt"),
                                   QJsonValue(static_cast<qint64>(
                                       unixSeconds(result.value->session.context.expiresAt)))},
                               {QStringLiteral("sessionId"),
                                   QJsonValue(static_cast<qint64>(result.value->session.context.sessionId))},
                               {QStringLiteral("admin"), adminJson(result.value->admin)},
                           });
                         });
      });

  routes.route("/admin/auth/reauth").methods(crow::HTTPMethod::POST)(
      [&auth, &sessions, &executor](
          const crow::request &request, crow::response &response) {
        crow::response failure;
        const auto context = requireAdmin(request, sessions, failure);
        if (!context) {
          response = std::move(failure);
          response.end();
          return;
        }
        const auto parsed = parseJsonObject(request, {"password"}, {"password"});
        if (!parsed.object || !parsed.object->value("password").isString()) {
          response = errorResponse(ErrorCode::InvalidArgument, "reauth request is invalid",
                                   "重新验证请求内容不符合要求");
          response.end();
          return;
        }
        const std::string password = parsed.object->value("password").toString().toStdString();
        dispatchBlocking(request, response, executor, [&auth, context = *context, password]() {
          const auto result =
              auth.reauthenticate(context, password, std::chrono::system_clock::now());
          if (!result.ok())
            return errorResponse(result.error, "", "");
          return successResponse(QJsonObject{
              {QStringLiteral("reauthExpiresAt"), QJsonValue(static_cast<qint64>(*result.value))}});
        });
      });

  routes.route("/admin/auth/logout").methods(crow::HTTPMethod::POST)(
      [&sessions](const crow::request &request) {
        crow::response failure;
        const auto context = requireAdmin(request, sessions, failure);
        if (!context)
          return failure;
        sessions.revoke(context->sessionId);
        return successResponse();
      });

  routes.route("/admin/users").methods(crow::HTTPMethod::GET)(
      [&users, &sessions, &executor](const crow::request &request,
                                    crow::response &response) {
        crow::response failure;
        if (!requireAdmin(request, sessions, failure)) {
          response = std::move(failure);
          response.end();
          return;
        }
        const auto pagination = adminPagination(
            request, {"status", "phoneExact", "phoneLast4"},
            {"registeredAt", "-registeredAt", "balanceCent", "-balanceCent"});
        if (!pagination) {
          response = errorResponse(ErrorCode::ValidationFailed,
                                   "unsupported paging or filter parameter",
                                   "分页或过滤参数不符合要求");
          response.end();
          return;
        }
        const auto status = parseIntegerFilter(*pagination, "status", 0, 1);
        const auto phoneExact = pagination->filters.find("phoneExact");
        const auto phoneLast4 = pagination->filters.find("phoneLast4");
        if (!status ||
            (phoneExact != pagination->filters.end() &&
             !decimalDigits(phoneExact->second, 11)) ||
            (phoneLast4 != pagination->filters.end() &&
             !decimalDigits(phoneLast4->second, 4))) {
          response = errorResponse(ErrorCode::ValidationFailed,
                                   "user filter is invalid", "用户过滤参数不符合要求");
          response.end();
          return;
        }
        core::application::AdminUserQuery query;
        if (*status)
          query.status = static_cast<int>(**status);
        if (pagination->filters.count("phoneExact"))
          query.phoneExact = pagination->filters.at("phoneExact");
        if (pagination->filters.count("phoneLast4"))
          query.phoneLast4 = pagination->filters.at("phoneLast4");
        query.sort = pagination->sort;
        query.page = pagination->page;
        query.pageSize = pagination->pageSize;
        dispatchBlocking(request, response, executor, [&users, query] {
          const auto result = users.list(query);
          QJsonArray items;
          for (const auto &user : result.items) {
            items.append(QJsonObject{
              {QStringLiteral("id"), QJsonValue(static_cast<qint64>(user.id))},
              {QStringLiteral("username"), QString::fromStdString(user.username)},
              {QStringLiteral("phoneMasked"), QString::fromStdString(maskedPhone(user.phone))},
              {QStringLiteral("nickname"), QString::fromStdString(user.nickname)},
              {QStringLiteral("status"), user.status},
              {QStringLiteral("balanceCent"), QJsonValue(static_cast<qint64>(user.balanceCent))},
              {QStringLiteral("debtCent"), QJsonValue(static_cast<qint64>(user.debtCent))},
              {QStringLiteral("registeredAt"), QJsonValue(static_cast<qint64>(user.registeredAt))},
            });
          }
          return successResponse(QJsonObject{
            {QStringLiteral("items"), items},
            {QStringLiteral("total"), result.total},
            {QStringLiteral("page"), result.page},
            {QStringLiteral("pageSize"), result.pageSize},
          });
        });
      });

  routes.route("/admin/users/<int>").methods(crow::HTTPMethod::GET)(
      [&users, &sessions, &executor](const crow::request &request,
                                    crow::response &response,
                                    std::int64_t userId) {
        crow::response failure;
        if (!requireAdmin(request, sessions, failure)) {
          response = std::move(failure);
          response.end();
          return;
        }
        dispatchBlocking(request, response, executor, [&users, userId] {
          const auto result = users.detail(userId, std::chrono::system_clock::now());
          if (!result.ok())
            return errorResponse(result.error, "", "");
          const auto &user = result.value->user;
          QJsonObject data{
            {QStringLiteral("id"), QJsonValue(static_cast<qint64>(user.id))},
            {QStringLiteral("username"), QString::fromStdString(user.username)},
            {QStringLiteral("phoneMasked"), QString::fromStdString(maskedPhone(user.phone))},
            {QStringLiteral("nickname"), QString::fromStdString(user.nickname)},
            {QStringLiteral("status"), user.status},
            {QStringLiteral("statusText"), user.status == 1 ? QStringLiteral("正常") : QStringLiteral("冻结")},
            {QStringLiteral("balanceCent"), QJsonValue(static_cast<qint64>(user.balanceCent))},
            {QStringLiteral("debtCent"), QJsonValue(static_cast<qint64>(user.debtCent))},
            {QStringLiteral("registeredAt"), QJsonValue(static_cast<qint64>(user.registeredAt))},
            {QStringLiteral("version"), QJsonValue(static_cast<qint64>(user.version))},
            {QStringLiteral("activeSessionCount"),
                QJsonValue(static_cast<qint64>(result.value->activeSessionCount))},
            {QStringLiteral("hasActiveFlow"), result.value->activeFlow.hasActiveFlow},
          };
          return successResponse(std::move(data));
        });
      });

  routes.route("/admin/users/<int>/status").methods(crow::HTTPMethod::PUT)(
      [&users, &sessions, &executor, &idempotency](
          const crow::request &request, crow::response &response, std::int64_t userId) {
        crow::response failure;
        const auto context = requireAdmin(request, sessions, failure);
        if (!context) {
          response = std::move(failure);
          response.end();
          return;
        }
        const auto actor = adminId(*context);
        if (!actor) {
          response = errorResponse(ErrorCode::Unauthorized, "authentication failed", "管理员凭据无效");
          response.end();
          return;
        }
        const auto parsed = parseJsonObject(request, {"status", "reason", "version"},
                                            {"status", "reason", "version"});
        if (!parsed.object || !validIntegerField(*parsed.object, "status", 0, 1) ||
            !parsed.object->value("reason").isString() ||
            !validIntegerField(*parsed.object, "version", 1, 9007199254740991LL)) {
          response = errorResponse(ErrorCode::ValidationFailed, "status update is invalid",
                                   "冻结或解冻请求内容不符合要求");
          response.end();
          return;
        }
        const std::int64_t actorValue = *actor;
        const int status = parsed.object->value("status").toInt();
        const std::string reason = parsed.object->value("reason").toString().toStdString();
        const std::int64_t version =
            static_cast<std::int64_t>(parsed.object->value("version").toDouble());
        dispatchIdempotentBlocking(
            request, response, executor, idempotency,
            "a" + std::to_string(actorValue) + ":user-status:" + std::to_string(userId), false,
            [&users, actorValue, userId, status, reason, version](
                core::application::IdempotencyLease &) {
              const auto result = users.updateStatus(
                  actorValue, userId, status, reason, version,
                  std::chrono::system_clock::now());
              if (!result.ok())
                return errorResponse(result.error, "", "");
              return successResponse(QJsonObject{
                  {QStringLiteral("id"), QJsonValue(static_cast<qint64>(result.value->user.id))},
                  {QStringLiteral("status"), result.value->user.status},
                  {QStringLiteral("statusText"),
                      result.value->user.status == 1 ? QStringLiteral("正常") : QStringLiteral("冻结")},
                  {QStringLiteral("version"),
                      QJsonValue(static_cast<qint64>(result.value->user.version))},
                  {QStringLiteral("activeFlowPreserved"), result.value->activeFlowPreserved}});
            });
      });

  routes.route("/admin/users/<int>/orders").methods(crow::HTTPMethod::GET)(
      [&users, &sessions, &executor](const crow::request &request,
                                    crow::response &response,
                                    std::int64_t userId) {
        crow::response failure;
        const auto context = requireAdmin(request, sessions, failure);
        if (!context) {
          response = std::move(failure);
          response.end();
          return;
        }
        const auto pagination = adminPagination(
            request, {"status", "fromAt", "toAt"},
            {"createdAt", "-createdAt"});
        if (!pagination) {
          response = errorResponse(ErrorCode::ValidationFailed,
                                   "unsupported paging or filter parameter",
                                   "分页或过滤参数不符合要求");
          response.end();
          return;
        }
        const auto actor = adminId(*context);
        const auto status = parseIntegerFilter(*pagination, "status", 60, 90);
        const auto fromAt = parseIntegerFilter(*pagination, "fromAt", 0, 4102444800LL);
        const auto toAt = parseIntegerFilter(*pagination, "toAt", 0, 4102444800LL);
        const bool statusAllowed = status &&
            (!*status || **status == 60 || **status == 70 || **status == 90);
        if (!actor || !statusAllowed || !fromAt || !toAt ||
            (*fromAt && *toAt && **fromAt > **toAt)) {
          response = errorResponse(ErrorCode::ValidationFailed,
                                   "order filters are invalid",
                                   "订单过滤参数不符合要求");
          response.end();
          return;
        }
        const std::int64_t actorValue = *actor;
        const auto statusValue = *status
            ? std::optional<int>(static_cast<int>(**status)) : std::nullopt;
        const std::int64_t fromValue = *fromAt ? **fromAt : 0;
        const std::int64_t toValue = *toAt ? **toAt : 0;
        const std::string sort = pagination->sort;
        const int page = pagination->page;
        const int pageSize = pagination->pageSize;
        dispatchBlocking(request, response, executor,
                         [&users, actorValue, userId, statusValue, fromValue,
                          toValue, sort, page, pageSize] {
          const auto result = users.orders(
              actorValue, userId, statusValue, fromValue, toValue, sort, page,
              pageSize, std::chrono::system_clock::now());
          if (!result.ok())
            return errorResponse(result.error, "", "");
          QJsonArray items;
          for (const auto &order : result.value->items) {
            items.append(QJsonObject{
              {QStringLiteral("orderNo"), QString::fromStdString(order.orderNo)},
              {QStringLiteral("flowNo"), QString::fromStdString(order.flowNo)},
              {QStringLiteral("stationName"), QString::fromStdString(order.stationName)},
              {QStringLiteral("chargerCode"), QString::fromStdString(order.chargerCode)},
              {QStringLiteral("status"), order.status},
              {QStringLiteral("statusText"), QString::fromStdString(order.statusText)},
              {QStringLiteral("energyMwh"), QJsonValue(static_cast<qint64>(order.energyMwh))},
              {QStringLiteral("amountCent"), QJsonValue(static_cast<qint64>(order.amountCent))},
            });
          }
          return successResponse(QJsonObject{
            {QStringLiteral("items"), items},
            {QStringLiteral("total"), result.value->total},
            {QStringLiteral("page"), result.value->page},
            {QStringLiteral("pageSize"), result.value->pageSize}});
        });
      });

  routes.route("/admin/stations").methods(crow::HTTPMethod::GET)(
      [&stations, &sessions, &executor](const crow::request &request,
                                       crow::response &response) {
        crow::response failure;
        if (!requireAdmin(request, sessions, failure)) {
          response = std::move(failure);
          response.end();
          return;
        }
        const auto pagination = adminPagination(request, {"status", "adcode", "keyword"});
        if (!pagination) {
          response = errorResponse(ErrorCode::ValidationFailed,
                                   "unsupported paging or filter parameter",
                                   "分页或过滤参数不符合要求");
          response.end();
          return;
        }
        const auto status = parseIntegerFilter(*pagination, "status", 0, 1);
        const auto adcode = pagination->filters.find("adcode");
        if (!status || (adcode != pagination->filters.end() &&
                        !decimalDigits(adcode->second, 6))) {
          response = errorResponse(ErrorCode::ValidationFailed,
                                   "station filters are invalid",
                                   "站点过滤参数不符合要求");
          response.end();
          return;
        }
        const auto statusValue = *status
            ? std::optional<int>(static_cast<int>(**status)) : std::nullopt;
        const auto adcodeValue = pagination->filters.count("adcode")
            ? std::optional<std::string>(pagination->filters.at("adcode"))
            : std::nullopt;
        const std::string keyword = pagination->filters.count("keyword")
            ? pagination->filters.at("keyword") : std::string();
        const int page = pagination->page;
        const int pageSize = pagination->pageSize;
        dispatchBlocking(request, response, executor,
                         [&stations, statusValue, adcodeValue, keyword, page,
                          pageSize] {
          const auto result = stations.stations(statusValue, adcodeValue, keyword,
                                                page, pageSize);
          QJsonArray items;
          for (const auto &station : result.items) {
            items.append(QJsonObject{
              {QStringLiteral("id"), QJsonValue(static_cast<qint64>(station.id))},
              {QStringLiteral("code"), QString::fromStdString(station.code)},
              {QStringLiteral("name"), QString::fromStdString(station.name)},
              {QStringLiteral("adcode"), QString::fromStdString(station.adcode)},
              {QStringLiteral("enabled"), station.enabled},
              {QStringLiteral("version"), QJsonValue(static_cast<qint64>(station.version))}});
          }
          return successResponse(QJsonObject{
            {QStringLiteral("items"), items},
            {QStringLiteral("total"), result.total},
            {QStringLiteral("page"), result.page},
            {QStringLiteral("pageSize"), result.pageSize}});
        });
      });

  routes.route("/admin/stations").methods(crow::HTTPMethod::POST)(
      [&stations, &sessions, &executor, &idempotency](
          const crow::request &request, crow::response &response) {
        crow::response failure;
        const auto context = requireAdmin(request, sessions, failure);
        if (!context) {
          response = std::move(failure);
          response.end();
          return;
        }
        const auto actor = adminId(*context);
        if (!actor) {
          response = errorResponse(ErrorCode::Unauthorized, "authentication failed", "管理员凭据无效");
          response.end();
          return;
        }
        const auto parsed = parseJsonObject(
            request,
            {"code", "name", "address", "adcode", "latitudeE6", "longitudeE6",
             "businessHours", "initialCharger"},
            {"code", "name", "address", "adcode", "latitudeE6", "longitudeE6", "initialCharger"});
        if (!parsed.object ||
            !validStringField(*parsed.object, "code", 2, 16) ||
            !validStringField(*parsed.object, "name", 1, 64) ||
            !validStringField(*parsed.object, "address", 1, 128) ||
            !validStringField(*parsed.object, "adcode", 6, 6) ||
            !validIntegerField(*parsed.object, "latitudeE6", -90000000, 90000000) ||
            !validIntegerField(*parsed.object, "longitudeE6", -180000000, 180000000) ||
            (parsed.object->contains("businessHours") &&
             !validStringField(*parsed.object, "businessHours", 0, 64)) ||
            !parsed.object->value("initialCharger").isObject()) {
          response = errorResponse(ErrorCode::ValidationFailed, "station payload is invalid",
                                   "新增站点请求内容不符合要求");
          response.end();
          return;
        }
        const QJsonObject initialCharger = parsed.object->value("initialCharger").toObject();
        if (!hasOnlyFields(initialCharger,
                           {"count", "chargerType", "powerWatt",
                            "connectorStandard"}) ||
            !validIntegerField(initialCharger, "count", 1, 100) ||
            !validIntegerField(initialCharger, "powerWatt", 1, 1000000) ||
            (initialCharger.contains("connectorStandard") &&
             !validStringField(initialCharger, "connectorStandard", 1, 32)) ||
            (initialCharger.value("chargerType").toInt(-1) != 0 &&
             initialCharger.value("chargerType").toInt(-1) != 1)) {
          response = errorResponse(ErrorCode::ValidationFailed, "station payload is invalid",
                                   "初始设备配置不符合要求");
          response.end();
          return;
        }
        core::application::Station draft;
        draft.code = parsed.object->value("code").toString().toStdString();
        draft.name = parsed.object->value("name").toString().toStdString();
        draft.address = parsed.object->value("address").toString().toStdString();
        draft.adcode = parsed.object->value("adcode").toString().toStdString();
        draft.latitudeE6 = static_cast<std::int64_t>(parsed.object->value("latitudeE6").toDouble());
        draft.longitudeE6 = static_cast<std::int64_t>(parsed.object->value("longitudeE6").toDouble());
        if (parsed.object->value("businessHours").isString())
          draft.businessHours = parsed.object->value("businessHours").toString().toStdString();
        core::application::InitialChargerSpec spec;
        spec.count = initialCharger.value("count").toInt();
        spec.chargerType = static_cast<core::application::ChargerType>(
            initialCharger.value("chargerType").toInt());
        spec.powerWatt = static_cast<std::int64_t>(initialCharger.value("powerWatt").toDouble());
        if (initialCharger.value("connectorStandard").isString())
          spec.connectorStandard =
              initialCharger.value("connectorStandard").toString().toStdString();
        const std::int64_t actorValue = *actor;
        dispatchIdempotentBlocking(
            request, response, executor, idempotency,
            "a" + std::to_string(actorValue) + ":station-create", false,
            [&stations, actorValue, draft, spec](core::application::IdempotencyLease &) {
              const auto result = stations.createStation(
                  actorValue, draft, spec, std::chrono::system_clock::now());
              if (!result.ok())
                return errorResponse(result.error, "", "");
              return successResponse(
                  QJsonObject{{QStringLiteral("id"), QJsonValue(static_cast<qint64>(result.value->id))},
                              {QStringLiteral("code"), QString::fromStdString(result.value->code)},
                              {QStringLiteral("enabled"), result.value->enabled},
                              {QStringLiteral("version"),
                                  QJsonValue(static_cast<qint64>(result.value->version))}},
                  201);
            });
      });

  routes.route("/admin/stations/<int>").methods(crow::HTTPMethod::PUT)(
      [&stations, &sessions, &executor, &idempotency](
          const crow::request &request, crow::response &response, std::int64_t stationId) {
        crow::response failure;
        const auto context = requireAdmin(request, sessions, failure);
        if (!context) {
          response = std::move(failure);
          response.end();
          return;
        }
        const auto actor = adminId(*context);
        const auto parsed = parseJsonObject(
            request,
            {"name", "address", "adcode", "latitudeE6", "longitudeE6",
             "businessHours", "version", "initialCharger"},
            {"version"});
        const bool noPatchFields = parsed.object && parsed.object->size() == 1;
        if (!parsed.object || !actor || noPatchFields ||
            !validIntegerField(*parsed.object, "version", 1, 9007199254740991LL) ||
            parsed.object->contains(QStringLiteral("initialCharger")) ||
            (parsed.object->contains("name") &&
             !validStringField(*parsed.object, "name", 1, 64)) ||
            (parsed.object->contains("address") &&
             !validStringField(*parsed.object, "address", 1, 128)) ||
            (parsed.object->contains("adcode") &&
             !validStringField(*parsed.object, "adcode", 6, 6)) ||
            (parsed.object->contains("latitudeE6") &&
             !validIntegerField(*parsed.object, "latitudeE6", -90000000, 90000000)) ||
            (parsed.object->contains("longitudeE6") &&
             !validIntegerField(*parsed.object, "longitudeE6", -180000000, 180000000)) ||
            (parsed.object->contains("businessHours") &&
             !validStringField(*parsed.object, "businessHours", 0, 64))) {
          response = errorResponse(ErrorCode::ValidationFailed, "station update is invalid",
                                   "修改站点请求内容不符合要求");
          response.end();
          return;
        }
        core::application::StationPatch patch;
        if (parsed.object->contains("name"))
          patch.name = parsed.object->value("name").toString().toStdString();
        if (parsed.object->contains("address"))
          patch.address = parsed.object->value("address").toString().toStdString();
        if (parsed.object->contains("adcode"))
          patch.adcode = parsed.object->value("adcode").toString().toStdString();
        if (parsed.object->contains("latitudeE6"))
          patch.latitudeE6 = static_cast<std::int64_t>(
              parsed.object->value("latitudeE6").toDouble());
        if (parsed.object->contains("longitudeE6"))
          patch.longitudeE6 = static_cast<std::int64_t>(
              parsed.object->value("longitudeE6").toDouble());
        if (parsed.object->contains("businessHours"))
          patch.businessHours =
              parsed.object->value("businessHours").toString().toStdString();
        const std::int64_t version =
            static_cast<std::int64_t>(parsed.object->value("version").toDouble());
        const std::int64_t actorValue = *actor;
        dispatchIdempotentBlocking(
            request, response, executor, idempotency,
            "a" + std::to_string(actorValue) + ":station-update:" +
                std::to_string(stationId),
            false,
                         [&stations, actorValue, stationId, patch, version](
                             core::application::IdempotencyLease &) {
                           const auto result = stations.updateStation(
                               actorValue, stationId, patch, version,
                               std::chrono::system_clock::now());
                           if (!result.ok())
                             return errorResponse(result.error, "", "");
                           return successResponse(QJsonObject{
                               {QStringLiteral("id"), QJsonValue(static_cast<qint64>(result.value->id))},
                               {QStringLiteral("version"),
                                   QJsonValue(static_cast<qint64>(result.value->version))}});
                         });
      });

  const auto stationEnabledRoute = [&](const bool enabled) {
    return [&stations, &sessions, &executor, &idempotency,
            enabled](const crow::request &request, crow::response &response,
                     std::int64_t stationId) {
      crow::response failure;
      const auto context = requireAdmin(request, sessions, failure);
      if (!context) {
        response = std::move(failure);
        response.end();
        return;
      }
      const auto actor = adminId(*context);
      const auto parsed = parseJsonObject(request, {"reason", "version"}, {"reason", "version"});
      if (!parsed.object || !actor || !parsed.object->value("reason").isString() ||
          !validIntegerField(*parsed.object, "version", 1, 9007199254740991LL)) {
        response = errorResponse(ErrorCode::ValidationFailed, "station state change is invalid",
                                 "站点启停请求内容不符合要求");
        response.end();
        return;
      }
      const std::string reason = parsed.object->value("reason").toString().toStdString();
      const std::int64_t version =
          static_cast<std::int64_t>(parsed.object->value("version").toDouble());
      const std::int64_t actorValue = *actor;
      dispatchIdempotentBlocking(
          request, response, executor, idempotency,
          "a" + std::to_string(actorValue) +
              (enabled ? ":station-enable:" : ":station-disable:") +
              std::to_string(stationId),
          false,
                       [&stations, actorValue, stationId, enabled, reason, version](
                           core::application::IdempotencyLease &) {
                         const auto result = stations.setStationEnabled(
                             actorValue, stationId, enabled, reason, version,
                             std::chrono::system_clock::now());
                         if (!result.ok())
                           return errorResponse(result.error, "", "");
                         return successResponse(QJsonObject{
                             {QStringLiteral("id"), QJsonValue(static_cast<qint64>(result.value->id))},
                             {QStringLiteral("enabled"), result.value->enabled},
                             {QStringLiteral("version"),
                                 QJsonValue(static_cast<qint64>(result.value->version))}});
                       });
    };
  };
  routes.route("/admin/stations/<int>/disable").methods(crow::HTTPMethod::POST)(
      stationEnabledRoute(false));
  routes.route("/admin/stations/<int>/enable").methods(crow::HTTPMethod::POST)(
      stationEnabledRoute(true));

  routes.route("/admin/chargers").methods(crow::HTTPMethod::GET)(
      [&stations, &sessions, &executor](const crow::request &request,
                                       crow::response &response) {
        crow::response failure;
        if (!requireAdmin(request, sessions, failure)) {
          response = std::move(failure);
          response.end();
          return;
        }
        const auto pagination = adminPagination(
            request, {"stationId", "status", "chargerType", "keyword"});
        if (!pagination) {
          response = errorResponse(ErrorCode::ValidationFailed,
                                   "unsupported paging or filter parameter",
                                   "分页或过滤参数不符合要求");
          response.end();
          return;
        }
        const auto stationId = parseIntegerFilter(
            *pagination, "stationId", 1, 9007199254740991LL);
        const auto status = parseIntegerFilter(*pagination, "status", 0, 4);
        const auto chargerType = parseIntegerFilter(*pagination, "chargerType", 0, 1);
        if (!stationId || !status || !chargerType) {
          response = errorResponse(ErrorCode::ValidationFailed,
                                   "charger filters are invalid",
                                   "设备过滤参数不符合要求");
          response.end();
          return;
        }
        std::optional<std::int64_t> stationValue;
        if (*stationId)
          stationValue = **stationId;
        const auto statusValue = *status
            ? std::optional<int>(static_cast<int>(**status)) : std::nullopt;
        const auto typeValue = *chargerType
            ? std::optional<int>(static_cast<int>(**chargerType)) : std::nullopt;
        const std::string keyword = pagination->filters.count("keyword")
            ? pagination->filters.at("keyword") : std::string();
        const int pageNumber = pagination->page;
        const int pageSize = pagination->pageSize;
        dispatchBlocking(request, response, executor,
                         [&stations, stationValue, statusValue, typeValue,
                          keyword, pageNumber, pageSize] {
          const auto page = stations.chargers(
              stationValue, statusValue, typeValue, keyword, pageNumber, pageSize);
          QJsonArray items;
          for (const auto &charger : page.items)
            items.append(chargerJson(charger));
          return successResponse(QJsonObject{
            {QStringLiteral("items"), items},
            {QStringLiteral("total"), page.total},
            {QStringLiteral("page"), page.page},
            {QStringLiteral("pageSize"), page.pageSize}});
        });
      });

  routes.route("/admin/chargers/batch").methods(crow::HTTPMethod::POST)(
      [&stations, &sessions, &executor, &idempotency](
          const crow::request &request, crow::response &response) {
        crow::response failure;
        const auto context = requireAdmin(request, sessions, failure);
        if (!context) {
          response = std::move(failure);
          response.end();
          return;
        }
        const auto actor = adminId(*context);
        const auto parsed = parseJsonObject(request, {"stationId", "chargers"},
                                            {"stationId", "chargers"});
        if (!parsed.object || !actor ||
            !validIntegerField(*parsed.object, "stationId", 1, 9007199254740991LL) ||
            !parsed.object->value("chargers").isArray() ||
            parsed.object->value("chargers").toArray().empty() ||
            parsed.object->value("chargers").toArray().size() > 100) {
          response = errorResponse(ErrorCode::ValidationFailed, "batch payload is invalid",
                                   "批量创建设备请求内容不符合要求");
          response.end();
          return;
        }
        std::vector<core::application::Charger> drafts;
        for (const auto &value : parsed.object->value("chargers").toArray()) {
          if (!value.isObject()) {
            response = errorResponse(ErrorCode::ValidationFailed, "batch payload is invalid",
                                     "批量创建设备请求内容不符合要求");
            response.end();
            return;
          }
          const QJsonObject item = value.toObject();
          if (!hasOnlyFields(item, {"code", "chargerType", "powerWatt",
                                    "connectorStandard"}) ||
              !validStringField(item, "code", 1, 32) ||
              (item.value("chargerType").toInt(-1) != 0 &&
               item.value("chargerType").toInt(-1) != 1) ||
              !validIntegerField(item, "powerWatt", 1, 1000000) ||
              (item.contains("connectorStandard") &&
               !validStringField(item, "connectorStandard", 1, 32))) {
            response = errorResponse(ErrorCode::ValidationFailed, "batch payload is invalid",
                                     "批量创建设备请求内容不符合要求");
            response.end();
            return;
          }
          core::application::Charger charger;
          charger.code = item.value("code").toString().toStdString();
          charger.type = static_cast<core::application::ChargerType>(
              item.value("chargerType").toInt());
          charger.powerWatt = static_cast<std::int64_t>(item.value("powerWatt").toDouble());
          if (item.value("connectorStandard").isString())
            charger.connectorStandard = item.value("connectorStandard").toString().toStdString();
          drafts.push_back(charger);
        }
        const std::int64_t stationId =
            static_cast<std::int64_t>(parsed.object->value("stationId").toDouble());
        const std::int64_t actorValue = *actor;
        dispatchIdempotentBlocking(
            request, response, executor, idempotency,
            "a" + std::to_string(actorValue) + ":charger-batch", false,
            [&stations, actorValue, stationId, drafts](core::application::IdempotencyLease &) {
              const auto result = stations.createChargers(
                  actorValue, stationId, drafts, std::chrono::system_clock::now());
              if (!result.ok())
                return errorResponse(result.error, "", "");
              QJsonArray items;
              for (const auto &charger : *result.value)
                items.append(QString::fromStdString(charger.code));
              return successResponse(QJsonObject{
                  {QStringLiteral("created"), items}}, 201);
            });
      });

  routes.route("/admin/chargers/<int>/status").methods(crow::HTTPMethod::PUT)(
      [&stations, &sessions, &executor, &idempotency](
          const crow::request &request, crow::response &response, std::int64_t chargerId) {
        crow::response failure;
        const auto context = requireAdmin(request, sessions, failure);
        if (!context) {
          response = std::move(failure);
          response.end();
          return;
        }
        const auto actor = adminId(*context);
        const auto parsed = parseJsonObject(request, {"targetStatus", "reason", "version"},
                                            {"targetStatus", "reason", "version"});
        if (!parsed.object || !actor ||
            !validIntegerField(*parsed.object, "targetStatus", 0, 3) ||
            !parsed.object->value("reason").isString() ||
            !validIntegerField(*parsed.object, "version", 1, 9007199254740991LL)) {
          response = errorResponse(ErrorCode::ValidationFailed, "charger status update is invalid",
                                   "设备状态变更请求内容不符合要求");
          response.end();
          return;
        }
        const int targetStatus = parsed.object->value("targetStatus").toInt();
        const std::string reason = parsed.object->value("reason").toString().toStdString();
        const std::int64_t version =
            static_cast<std::int64_t>(parsed.object->value("version").toDouble());
        const std::int64_t actorValue = *actor;
        dispatchIdempotentBlocking(
            request, response, executor, idempotency,
            "a" + std::to_string(actorValue) + ":charger-status:" + std::to_string(chargerId),
            false,
            [&stations, actorValue, chargerId, targetStatus, reason, version](
                core::application::IdempotencyLease &) {
              const auto result = stations.setChargerStatus(
                  actorValue, chargerId, targetStatus, reason, version,
                  std::chrono::system_clock::now());
              if (!result.ok())
                return errorResponse(result.error, "", "");
              return successResponse(chargerJson(*result.value));
            });
      });

  routes.route("/admin/chargers/<int>/restart-commands").methods(crow::HTTPMethod::POST)(
      [&stations, &sessions, &executor, &idempotency](
          const crow::request &request, crow::response &response, std::int64_t chargerId) {
        crow::response failure;
        const auto context = middleware::authorize(
            request, sessions, {core::application::TokenKind::Administrator},
            {core::application::Role::Operator, core::application::Role::Owner},
            std::chrono::system_clock::now(), true);
        if (!context.context) {
          response = errorResponse(context.error, "reauthentication or credentials required",
                                   "需要重新验证管理员密码");
          response.end();
          return;
        }
        const auto actor = adminId(*context.context);
        const auto parsed = parseJsonObject(request, {"confirm", "reason"}, {"confirm", "reason"});
        if (!parsed.object || !actor || !parsed.object->value("confirm").isBool() ||
            !parsed.object->value("confirm").toBool() ||
            !parsed.object->value("reason").isString()) {
          response = errorResponse(ErrorCode::ValidationFailed, "restart command is invalid",
                                   "远程重启请求必须二次确认并填写原因");
          response.end();
          return;
        }
        const std::string reason = parsed.object->value("reason").toString().toStdString();
        const std::int64_t actorValue = *actor;
        dispatchIdempotentBlocking(
            request, response, executor, idempotency,
            "a" + std::to_string(actorValue) + ":restart:" + std::to_string(chargerId), false,
            [&stations, actorValue, chargerId, reason](core::application::IdempotencyLease &) {
              const auto result = stations.createRestartCommand(
                  actorValue, chargerId, reason, std::chrono::system_clock::now());
              if (!result.ok())
                return errorResponse(result.error, "", "");
              return successResponse(
                  QJsonObject{{QStringLiteral("commandNo"),
                                  QString::fromStdString(result.value->commandNo)},
                              {QStringLiteral("status"), QString::fromStdString(result.value->status)},
                              {QStringLiteral("chargerStatus"), result.value->chargerStatus},
                              {QStringLiteral("createdAt"),
                                  QJsonValue(static_cast<qint64>(result.value->createdAt))}},
                  202);
            });
      });

  routes.route("/admin/device-commands/<string>").methods(crow::HTTPMethod::GET)(
      [&stations, &sessions, &executor](const crow::request &request,
                                       crow::response &response,
                                       std::string commandNo) {
        crow::response failure;
        if (!requireAdmin(request, sessions, failure)) {
          response = std::move(failure);
          response.end();
          return;
        }
        dispatchBlocking(request, response, executor,
                         [&stations, commandNo = std::move(commandNo)] {
          const auto result =
              stations.deviceCommand(commandNo, std::chrono::system_clock::now());
          if (!result.ok())
            return errorResponse(result.error, "", "");
          return successResponse(deviceCommandJson(*result.value));
        });
      });

  routes.route("/admin/tariffs").methods(crow::HTTPMethod::GET)(
      [&stations, &sessions, &executor](const crow::request &request,
                                       crow::response &response) {
        crow::response failure;
        if (!requireAdmin(request, sessions, failure)) {
          response = std::move(failure);
          response.end();
          return;
        }
        const auto pagination = adminPagination(request, {"adcode", "effectiveAt"});
        if (!pagination) {
          response = errorResponse(ErrorCode::ValidationFailed,
                                   "unsupported paging or filter parameter",
                                   "分页或过滤参数不符合要求");
          response.end();
          return;
        }
        const auto effectiveAt = parseIntegerFilter(
            *pagination, "effectiveAt", 0, 4102444800LL);
        const auto adcode = pagination->filters.find("adcode");
        if (!effectiveAt || (adcode != pagination->filters.end() &&
                             !decimalDigits(adcode->second, 6))) {
          response = errorResponse(ErrorCode::ValidationFailed,
                                   "tariff filters are invalid",
                                   "价格过滤参数不符合要求");
          response.end();
          return;
        }
        const auto adcodeValue = pagination->filters.count("adcode")
            ? std::optional<std::string>(pagination->filters.at("adcode"))
            : std::nullopt;
        std::optional<std::int64_t> effectiveValue;
        if (*effectiveAt)
          effectiveValue = **effectiveAt;
        const int page = pagination->page;
        const int pageSize = pagination->pageSize;
        dispatchBlocking(request, response, executor,
                         [&stations, adcodeValue, effectiveValue, page, pageSize] {
          const auto all = stations.tariffs(adcodeValue);
          std::vector<core::application::RegionTariff> filtered;
          for (const auto &tariff : all) {
            if (effectiveValue &&
                (tariff.effectiveFrom > *effectiveValue ||
                 tariff.effectiveTo <= *effectiveValue))
              continue;
            filtered.push_back(tariff);
          }
          QJsonArray array;
          const std::size_t first = static_cast<std::size_t>(page - 1) * pageSize;
          for (std::size_t index = first;
               index < filtered.size() &&
               index < first + static_cast<std::size_t>(pageSize); ++index)
            array.append(tariffJson(filtered[index]));
          return successResponse(QJsonObject{
            {QStringLiteral("items"), array},
            {QStringLiteral("total"), static_cast<int>(filtered.size())},
            {QStringLiteral("page"), page},
            {QStringLiteral("pageSize"), pageSize}});
        });
      });

  routes.route("/admin/tariffs").methods(crow::HTTPMethod::POST)(
      [&stations, &sessions, &executor, &idempotency](
          const crow::request &request, crow::response &response) {
        crow::response failure;
        const auto context = requireAdmin(request, sessions, failure);
        if (!context) {
          response = std::move(failure);
          response.end();
          return;
        }
        const auto actor = adminId(*context);
        const auto parsed = parseJsonObject(
            request,
            {"adcode", "electricityPriceCentPerKwh", "servicePriceCentPerKwh",
             "effectiveFrom", "effectiveTo", "reason"},
            {"adcode", "electricityPriceCentPerKwh", "servicePriceCentPerKwh",
             "effectiveFrom", "reason"});
        const bool hasEffectiveTo = parsed.object && parsed.object->contains("effectiveTo") &&
                                    !parsed.object->value("effectiveTo").isNull();
        if (!parsed.object || !actor ||
            !validStringField(*parsed.object, "adcode", 6, 6) ||
            !validIntegerField(*parsed.object, "electricityPriceCentPerKwh", 0, 100000) ||
            !validIntegerField(*parsed.object, "servicePriceCentPerKwh", 0, 100000) ||
            !validIntegerField(*parsed.object, "effectiveFrom", 0, 4102444800LL) ||
            (hasEffectiveTo && !validIntegerField(*parsed.object, "effectiveTo", 1, 4102444800LL)) ||
            !validStringField(*parsed.object, "reason", 2, 200)) {
          response = errorResponse(ErrorCode::ValidationFailed, "tariff payload is invalid",
                                   "价格版本请求内容不符合要求");
          response.end();
          return;
        }
        core::application::RegionTariff draft;
        draft.adcode = parsed.object->value("adcode").toString().toStdString();
        draft.electricityCentPerKwh =
            parsed.object->value("electricityPriceCentPerKwh").toInt();
        draft.serviceCentPerKwh = parsed.object->value("servicePriceCentPerKwh").toInt();
        draft.effectiveFrom =
            static_cast<std::int64_t>(parsed.object->value("effectiveFrom").toDouble());
        if (hasEffectiveTo)
          draft.effectiveTo =
              static_cast<std::int64_t>(parsed.object->value("effectiveTo").toDouble());
        if (!decimalDigits(draft.adcode, 6) ||
            (hasEffectiveTo && draft.effectiveTo <= draft.effectiveFrom)) {
          response = errorResponse(ErrorCode::ValidationFailed, "tariff payload is invalid",
                                   "价格版本请求内容不符合要求");
          response.end();
          return;
        }
        const std::string reason = parsed.object->value("reason").toString().toStdString();
        const std::int64_t actorValue = *actor;
        dispatchIdempotentBlocking(
            request, response, executor, idempotency,
            "a" + std::to_string(actorValue) + ":tariff-create", false,
                         [&stations, actorValue, draft, reason](
                             core::application::IdempotencyLease &) {
                           const auto result = stations.createTariff(
                               actorValue, draft, reason, std::chrono::system_clock::now());
                           if (!result.ok())
                             return errorResponse(result.error, "", "");
                           return successResponse(tariffJson(*result.value), 201);
                         });
      });

  routes.route("/admin/price-adjustments").methods(crow::HTTPMethod::POST)(
      [&stations, &sessions, &executor, &idempotency](
          const crow::request &request, crow::response &response) {
        crow::response failure;
        const auto context = requireAdmin(request, sessions, failure);
        if (!context) {
          response = std::move(failure);
          response.end();
          return;
        }
        const auto actor = adminId(*context);
        const auto parsed = parseJsonObject(
            request,
            {"stationId", "chargerType", "source", "adjustmentBp", "effectiveFrom",
             "effectiveTo", "reason"},
            {"stationId", "chargerType", "source", "adjustmentBp", "effectiveFrom",
             "effectiveTo", "reason"});
        if (!parsed.object || !actor ||
            !validIntegerField(*parsed.object, "stationId", 1, 9007199254740991LL) ||
            !validIntegerField(*parsed.object, "chargerType", 0, 1) ||
            !validStringField(*parsed.object, "source", 1, 32) ||
            !validIntegerField(*parsed.object, "adjustmentBp", -2000, 2000) ||
            !validIntegerField(*parsed.object, "effectiveFrom", 0, 4102444800LL) ||
            !validIntegerField(*parsed.object, "effectiveTo", 0, 4102444800LL) ||
            !validStringField(*parsed.object, "reason", 2, 200)) {
          response = errorResponse(ErrorCode::ValidationFailed, "adjustment payload is invalid",
                                   "服务费调整请求内容不符合要求");
          response.end();
          return;
        }
        core::application::PriceAdjustment draft;
        draft.stationId =
            static_cast<std::int64_t>(parsed.object->value("stationId").toDouble());
        draft.chargerType = parsed.object->value("chargerType").toInt();
        draft.source = parsed.object->value("source").toString().toStdString();
        draft.adjustmentBp = parsed.object->value("adjustmentBp").toInt();
        draft.effectiveFrom =
            static_cast<std::int64_t>(parsed.object->value("effectiveFrom").toDouble());
        draft.effectiveTo =
            static_cast<std::int64_t>(parsed.object->value("effectiveTo").toDouble());
        const std::string reason = parsed.object->value("reason").toString().toStdString();
        if ((draft.source != "ML_APPROVED" && draft.source != "MANUAL") ||
            draft.adjustmentBp % 500 != 0 ||
            draft.effectiveTo <= draft.effectiveFrom) {
          response = errorResponse(ErrorCode::ValidationFailed, "adjustment payload is invalid",
                                   "服务费调整请求内容不符合要求");
          response.end();
          return;
        }
        const std::int64_t actorValue = *actor;
        dispatchIdempotentBlocking(
            request, response, executor, idempotency,
            "a" + std::to_string(actorValue) + ":price-adjustment", false,
                         [&stations, actorValue, draft, reason](
                             core::application::IdempotencyLease &) {
                           const auto result = stations.createPriceAdjustment(
                               actorValue, draft, reason, std::chrono::system_clock::now());
                           if (!result.ok())
                             return errorResponse(result.error, "", "");
                           return successResponse(QJsonObject{
                               {QStringLiteral("id"),
                                   QJsonValue(static_cast<qint64>(result.value->id))},
                               {QStringLiteral("adjustmentBp"), result.value->adjustmentBp}},
                               201);
                         });
      });

  routes.route("/admin/flows").methods(crow::HTTPMethod::GET)(
      [&ops, &sessions, &executor](const crow::request &request,
                                  crow::response &response) {
        crow::response failure;
        if (!requireAdmin(request, sessions, failure)) {
          response = std::move(failure);
          response.end();
          return;
        }
        const auto pagination = adminPagination(
            request, {"status", "stationId", "chargerId", "userId"});
        if (!pagination) {
          response = errorResponse(ErrorCode::ValidationFailed,
                                   "unsupported paging or filter parameter",
                                   "分页或过滤参数不符合要求");
          response.end();
          return;
        }
        const auto status = parseIntegerFilter(*pagination, "status", 10, 90);
        const auto stationId = parseIntegerFilter(
            *pagination, "stationId", 1, 9007199254740991LL);
        const auto chargerId = parseIntegerFilter(
            *pagination, "chargerId", 1, 9007199254740991LL);
        const auto userId = parseIntegerFilter(
            *pagination, "userId", 1, 9007199254740991LL);
        const bool statusAllowed = status &&
            (!*status || **status == 10 || **status == 20 || **status == 30 ||
             **status == 40 || **status == 50 || **status == 60 ||
             **status == 70 || **status == 80 || **status == 90);
        if (!statusAllowed || !stationId || !chargerId || !userId) {
          response = errorResponse(ErrorCode::ValidationFailed,
                                   "flow filters are invalid",
                                   "流程过滤参数不符合要求");
          response.end();
          return;
        }
        core::application::AdminFlowQuery query;
        if (*status)
          query.status = static_cast<int>(**status);
        if (*stationId)
          query.stationId = **stationId;
        if (*chargerId)
          query.chargerId = **chargerId;
        if (*userId)
          query.userId = **userId;
        query.page = pagination->page;
        query.pageSize = pagination->pageSize;
        dispatchBlocking(request, response, executor, [&ops, query] {
          const auto page = ops.flows(query);
          QJsonArray items;
          for (const auto &flow : page.items)
            items.append(flowJson(flow));
          return successResponse(QJsonObject{
            {QStringLiteral("items"), items},
            {QStringLiteral("total"), page.total},
            {QStringLiteral("page"), page.page},
            {QStringLiteral("pageSize"), page.pageSize}});
        });
      });

  routes.route("/admin/flows/<string>/force-releases").methods(crow::HTTPMethod::POST)(
      [&ops, &sessions, &executor, &idempotency](
          const crow::request &request, crow::response &response, std::string flowNo) {
        crow::response failure;
        const auto context = middleware::authorize(
            request, sessions, {core::application::TokenKind::Administrator},
            {core::application::Role::Operator, core::application::Role::Owner},
            std::chrono::system_clock::now(), true);
        if (!context.context) {
          response = errorResponse(context.error, "reauthentication or credentials required",
                                   "需要重新验证管理员密码");
          response.end();
          return;
        }
        const auto actor = adminId(*context.context);
        const auto parsed = parseJsonObject(
            request, {"confirm", "reason", "nextChargerStatus", "flowVersion"},
            {"confirm", "reason", "nextChargerStatus", "flowVersion"});
        if (!parsed.object || !actor || !parsed.object->value("confirm").isBool() ||
            !parsed.object->value("confirm").toBool() ||
            !parsed.object->value("reason").isString() ||
            !validIntegerField(*parsed.object, "nextChargerStatus", 0, 3) ||
            !validIntegerField(*parsed.object, "flowVersion", 1, 9007199254740991LL)) {
          response = errorResponse(ErrorCode::ValidationFailed, "force release is invalid",
                                   "强制释放请求必须二次确认并填写原因");
          response.end();
          return;
        }
        const std::string reason = parsed.object->value("reason").toString().toStdString();
        const int nextStatus = parsed.object->value("nextChargerStatus").toInt();
        const std::int64_t flowVersion =
            static_cast<std::int64_t>(parsed.object->value("flowVersion").toDouble());
        const std::int64_t actorValue = *actor;
        dispatchIdempotentBlocking(
            request, response, executor, idempotency,
            "a" + std::to_string(actorValue) + ":force-release:" + flowNo, false,
            [&ops, actorValue, flowNo, reason, nextStatus, flowVersion](
                core::application::IdempotencyLease &) {
              const auto result = ops.forceRelease(
                  actorValue, flowNo, reason, nextStatus, flowVersion,
                  std::chrono::system_clock::now());
              if (!result.ok())
                return errorResponse(result.error, "", "");
              return successResponse(QJsonObject{
                  {QStringLiteral("flowNo"), QString::fromStdString(result.value->flowNo)},
                  {QStringLiteral("status"), result.value->status},
                  {QStringLiteral("statusText"), QString::fromStdString(result.value->statusText)},
                  {QStringLiteral("version"), QJsonValue(static_cast<qint64>(result.value->version))}});
            });
      });

  routes.route("/admin/stats/revenue").methods(crow::HTTPMethod::GET)(
      [&ops, &sessions, &executor](const crow::request &request,
                                  crow::response &response) {
        crow::response failure;
        if (!requireAdmin(request, sessions, failure)) {
          response = std::move(failure);
          response.end();
          return;
        }
        const auto parameters = parseQueryParameters(
            request, {"fromAt", "toAt", "stationId", "bucket"});
        const auto fromAt = parameters
            ? parseIntegerParameter(*parameters, "fromAt", 0, 4102444800LL)
            : std::optional<std::optional<std::int64_t>>{};
        const auto toAt = parameters
            ? parseIntegerParameter(*parameters, "toAt", 0, 4102444800LL)
            : std::optional<std::optional<std::int64_t>>{};
        const auto stationId = parameters
            ? parseIntegerParameter(*parameters, "stationId", 1,
                                    9007199254740991LL)
            : std::optional<std::optional<std::int64_t>>{};
        const std::string bucket = parameters && parameters->count("bucket")
            ? parameters->at("bucket") : "day";
        if (!parameters || !fromAt || !toAt || !stationId ||
            !core::application::AdminOpsService::validBucket(bucket) ||
            (*fromAt && *toAt && **fromAt > **toAt)) {
          response = errorResponse(ErrorCode::ValidationFailed,
                                   "statistics parameters are invalid",
                                   "统计参数不符合要求");
          response.end();
          return;
        }
        const std::int64_t fromValue = *fromAt ? **fromAt : 0;
        const std::int64_t toValue = *toAt ? **toAt : 0;
        std::optional<std::int64_t> stationValue;
        if (*stationId)
          stationValue = **stationId;
        dispatchBlocking(request, response, executor,
                         [&ops, fromValue, toValue, stationValue, bucket] {
          const auto result = ops.revenueStats(fromValue, toValue, stationValue,
                                               bucket);
          if (!result.ok())
            return errorResponse(result.error, "", "");
          QJsonArray items;
          for (const auto &entry : result.value->items) {
            items.append(QJsonObject{
                {QStringLiteral("bucketStart"), QJsonValue(static_cast<qint64>(entry.bucketStart))},
                {QStringLiteral("amountCent"), QJsonValue(static_cast<qint64>(entry.amountCent))},
                {QStringLiteral("energyMwh"), QJsonValue(static_cast<qint64>(entry.energyMwh))},
                {QStringLiteral("orderCount"), entry.orderCount}});
          }
          return successResponse(QJsonObject{
              {QStringLiteral("items"), items},
              {QStringLiteral("totalAmountCent"),
                  QJsonValue(static_cast<qint64>(result.value->totalAmountCent))},
              {QStringLiteral("totalEnergyMwh"),
                  QJsonValue(static_cast<qint64>(result.value->totalEnergyMwh))},
              {QStringLiteral("totalOrderCount"), result.value->totalOrderCount}});
        });
      });

  routes.route("/admin/stats/charger-status").methods(crow::HTTPMethod::GET)(
      [&ops, &sessions, &executor](const crow::request &request,
                                  crow::response &response) {
        crow::response failure;
        if (!requireAdmin(request, sessions, failure)) {
          response = std::move(failure);
          response.end();
          return;
        }
        const auto parameters = parseQueryParameters(request, {"stationId"});
        const auto stationId = parameters
            ? parseIntegerParameter(*parameters, "stationId", 1,
                                    9007199254740991LL)
            : std::optional<std::optional<std::int64_t>>{};
        if (!parameters || !stationId) {
          response = errorResponse(ErrorCode::ValidationFailed,
                                   "statistics parameters are invalid",
                                   "统计参数不符合要求");
          response.end();
          return;
        }
        std::optional<std::int64_t> stationValue;
        if (*stationId)
          stationValue = **stationId;
        dispatchBlocking(request, response, executor, [&ops, stationValue] {
          const auto stats = ops.chargerStatusStats(stationValue);
          return successResponse(QJsonObject{
              {QStringLiteral("idleCount"), stats.idleCount},
              {QStringLiteral("occupiedCount"), stats.occupiedCount},
              {QStringLiteral("faultyCount"), stats.faultyCount},
              {QStringLiteral("restartingCount"), stats.restartingCount},
              {QStringLiteral("disabledCount"), stats.disabledCount},
              {QStringLiteral("operationalCount"), stats.operationalCount},
              {QStringLiteral("totalCount"), stats.totalCount},
              {QStringLiteral("healthPercent"), stats.healthPercent}});
        });
      });

  routes.route("/admin/audit-logs").methods(crow::HTTPMethod::GET)(
      [&ops, &sessions, &executor](const crow::request &request,
                                  crow::response &response) {
        crow::response failure;
        if (!requireOwner(request, sessions, failure)) {
          response = std::move(failure);
          response.end();
          return;
        }
        const auto pagination = adminPagination(
            request, {"actorId", "action", "targetType", "targetId", "fromAt", "toAt"});
        if (!pagination) {
          response = errorResponse(ErrorCode::ValidationFailed,
                                   "unsupported paging or filter parameter",
                                   "分页或过滤参数不符合要求");
          response.end();
          return;
        }
        const auto fromAt = parseIntegerFilter(*pagination, "fromAt", 0, 4102444800LL);
        const auto toAt = parseIntegerFilter(*pagination, "toAt", 0, 4102444800LL);
        const auto actorFilter = pagination->filters.find("actorId");
        bool actorValid = true;
        if (actorFilter != pagination->filters.end() && !actorFilter->second.empty()) {
          std::string_view actor = actorFilter->second;
          if (actor.rfind("admin:", 0) == 0)
            actor.remove_prefix(6);
          actorValid = !actor.empty() && actor.size() <= 16 &&
                       std::all_of(actor.begin(), actor.end(), [](unsigned char value) {
                         return std::isdigit(value) != 0;
                       });
        }
        if (!fromAt || !toAt || !actorValid ||
            (*fromAt && *toAt && **fromAt > **toAt)) {
          response = errorResponse(ErrorCode::ValidationFailed,
                                   "audit filters are invalid",
                                   "审计过滤参数不符合要求");
          response.end();
          return;
        }
        core::application::AuditEventQuery query;
        if (pagination->filters.count("actorId"))
          query.actorId = pagination->filters.at("actorId");
        if (pagination->filters.count("action"))
          query.action = pagination->filters.at("action");
        if (pagination->filters.count("targetType"))
          query.targetType = pagination->filters.at("targetType");
        if (pagination->filters.count("targetId"))
          query.targetId = pagination->filters.at("targetId");
        if (*fromAt)
          query.fromAt = **fromAt;
        if (*toAt)
          query.toAt = **toAt;
        query.page = pagination->page;
        query.pageSize = pagination->pageSize;
        dispatchBlocking(request, response, executor, [&ops, query] {
          const auto events = ops.auditEvents(query);
          QJsonArray items;
          for (const auto &event : events)
            items.append(auditJson(event));
          return successResponse(QJsonObject{
            {QStringLiteral("items"), items},
            {QStringLiteral("page"), query.page},
            {QStringLiteral("pageSize"), query.pageSize}});
        });
      });

  routes.route("/admin/backups").methods(crow::HTTPMethod::POST)(
      [&ops, &sessions, &executor, &idempotency](
          const crow::request &request, crow::response &response) {
        crow::response failure;
        const auto context = middleware::authorize(
            request, sessions, {core::application::TokenKind::Administrator},
            {core::application::Role::Owner}, std::chrono::system_clock::now(), true);
        if (!context.context) {
          response = errorResponse(context.error, "owner role or reauthentication required",
                                   "需要 OWNER 权限并重新验证");
          response.end();
          return;
        }
        const auto actor = adminId(*context.context);
        if (!actor) {
          response = errorResponse(ErrorCode::Unauthorized, "authentication failed", "管理员凭据无效");
          response.end();
          return;
        }
        const std::int64_t actorValue = *actor;
        dispatchIdempotentBlocking(
            request, response, executor, idempotency,
            "a" + std::to_string(actorValue) + ":backup", false,
            [&ops, actorValue](core::application::IdempotencyLease &) {
              const auto result =
                  ops.createBackup(actorValue, std::chrono::system_clock::now());
              if (!result.ok())
                return errorResponse(result.error, "", "");
              return successResponse(QJsonObject{
                  {QStringLiteral("backupNo"), QString::fromStdString(result.value->backupNo)},
                  {QStringLiteral("status"), QString::fromStdString(result.value->status)},
                  {QStringLiteral("checksum"), QString::fromStdString(result.value->checksum)}},
                  202);
            });
      });

  routes.route("/admin/backups").methods(crow::HTTPMethod::GET)(
      [&ops, &sessions, &executor](const crow::request &request,
                                  crow::response &response) {
        crow::response failure;
        if (!requireOwner(request, sessions, failure)) {
          response = std::move(failure);
          response.end();
          return;
        }
        dispatchBlocking(request, response, executor, [&ops] {
          const auto records = ops.backups();
          QJsonArray items;
          for (const auto &record : records) {
            items.append(QJsonObject{
              {QStringLiteral("backupNo"), QString::fromStdString(record.backupNo)},
              {QStringLiteral("status"), QString::fromStdString(record.status)},
              {QStringLiteral("checksum"), QString::fromStdString(record.checksum)},
              {QStringLiteral("sizeBytes"), QJsonValue(static_cast<qint64>(record.sizeBytes))},
              {QStringLiteral("createdAt"), QJsonValue(static_cast<qint64>(record.createdAt))},
              {QStringLiteral("verificationStatus"),
                  QString::fromStdString(record.verificationStatus)}});
          }
          return successResponse(QJsonObject{{QStringLiteral("items"), items}});
        });
      });

  routes.route("/admin/backups/<string>/verifications").methods(crow::HTTPMethod::POST)(
      [&ops, &sessions, &executor, &idempotency](
          const crow::request &request, crow::response &response,
          std::string backupNo) {
        crow::response failure;
        const auto context = requireOwner(request, sessions, failure);
        if (!context) {
          response = std::move(failure);
          response.end();
          return;
        }
        const auto actor = adminId(*context);
        if (!actor || backupNo.empty() || backupNo.size() > 64) {
          response = errorResponse(ErrorCode::ValidationFailed,
                                   "backup verification is invalid",
                                   "备份验证请求不符合要求");
          response.end();
          return;
        }
        const std::int64_t actorValue = *actor;
        dispatchIdempotentBlocking(
            request, response, executor, idempotency,
            "a" + std::to_string(actorValue) + ":backup-verify:" + backupNo,
            false,
            [&ops, actorValue, backupNo = std::move(backupNo)](
                core::application::IdempotencyLease &) {
              const auto result = ops.verifyBackup(
                  actorValue, backupNo, std::chrono::system_clock::now());
              if (!result.ok())
                return errorResponse(result.error, "", "");
              return successResponse(QJsonObject{
                  {QStringLiteral("backupNo"), QString::fromStdString(result.value->backupNo)},
                  {QStringLiteral("verificationStatus"),
                      QString::fromStdString(result.value->verificationStatus)}},
                  202);
            });
      });

  routes.route("/admin/predictions").methods(crow::HTTPMethod::GET)(
      [&ops, &sessions, &executor](const crow::request &request,
                                  crow::response &response) {
        crow::response failure;
        if (!requireAdmin(request, sessions, failure)) {
          response = std::move(failure);
          response.end();
          return;
        }
        const auto parameters = parseQueryParameters(
            request, {"stationId", "horizonHour", "fromAt"});
        const auto stationId = parameters
            ? parseIntegerParameter(*parameters, "stationId", 1,
                                    9007199254740991LL)
            : std::optional<std::optional<std::int64_t>>{};
        const auto horizon = parameters
            ? parseIntegerParameter(*parameters, "horizonHour", 1, 24)
            : std::optional<std::optional<std::int64_t>>{};
        const auto fromAt = parameters
            ? parseIntegerParameter(*parameters, "fromAt", 0, 4102444800LL)
            : std::optional<std::optional<std::int64_t>>{};
        const bool horizonAllowed = horizon &&
            (!*horizon || **horizon == 1 || **horizon == 6 || **horizon == 24);
        if (!parameters || !stationId || !horizonAllowed || !fromAt) {
          response = errorResponse(ErrorCode::ValidationFailed,
                                   "prediction parameters are invalid",
                                   "预测参数不符合要求");
          response.end();
          return;
        }
        std::optional<std::int64_t> stationValue;
        if (*stationId)
          stationValue = **stationId;
        std::optional<int> horizonValue;
        if (*horizon)
          horizonValue = static_cast<int>(**horizon);
        const std::int64_t fromValue = *fromAt ? **fromAt : 0;
        dispatchBlocking(request, response, executor,
                         [&ops, stationValue, horizonValue, fromValue] {
          const auto items = ops.predictions(stationValue, horizonValue, fromValue);
          QJsonArray array;
          for (const auto &prediction : items) {
            array.append(QJsonObject{
              {QStringLiteral("stationId"), QJsonValue(static_cast<qint64>(prediction.stationId))},
              {QStringLiteral("horizonHour"), prediction.horizonHour},
              {QStringLiteral("modelVersion"), QString::fromStdString(prediction.modelVersion)},
              {QStringLiteral("generatedAt"), QJsonValue(static_cast<qint64>(prediction.generatedAt))},
              {QStringLiteral("targetAt"), QJsonValue(static_cast<qint64>(prediction.targetAt))},
              {QStringLiteral("predictedEnergyMwh"),
                  QJsonValue(static_cast<qint64>(prediction.predictedEnergyMwh))},
              {QStringLiteral("predictedIdleCount"), prediction.predictedIdleCount},
              {QStringLiteral("peakFlag"), prediction.peakFlag},
              {QStringLiteral("staleFlag"), prediction.staleFlag}});
          }
          return successResponse(QJsonObject{{QStringLiteral("items"), array}});
        });
      });

  routes.route("/admin/ml-tasks").methods(crow::HTTPMethod::POST)(
      [&ops, &sessions, &executor, &idempotency](
          const crow::request &request, crow::response &response) {
        crow::response failure;
        const auto context = requireAdmin(request, sessions, failure);
        if (!context) {
          response = std::move(failure);
          response.end();
          return;
        }
        const auto actor = adminId(*context);
        const auto parsed = parseJsonObject(request, {"taskType", "horizonHours"},
                                            {"taskType"});
        if (!parsed.object || !actor ||
            !validStringField(*parsed.object, "taskType", 5, 7) ||
            (parsed.object->contains("horizonHours") &&
             !parsed.object->value("horizonHours").isArray())) {
          response = errorResponse(ErrorCode::ValidationFailed, "ml task payload is invalid",
                                   "ML 任务请求内容不符合要求");
          response.end();
          return;
        }
        std::vector<int> horizonHours;
        if (parsed.object->value("horizonHours").isArray()) {
          for (const auto &value : parsed.object->value("horizonHours").toArray()) {
            if (!value.isDouble() || value.toDouble() != value.toInt() ||
                (value.toInt() != 1 && value.toInt() != 6 && value.toInt() != 24) ||
                std::find(horizonHours.begin(), horizonHours.end(), value.toInt()) !=
                    horizonHours.end()) {
              response = errorResponse(ErrorCode::ValidationFailed,
                                       "ml horizons are invalid",
                                       "ML 任务预测周期不符合要求");
              response.end();
              return;
            }
            horizonHours.push_back(value.toInt());
          }
        }
        const std::string taskType =
            parsed.object->value("taskType").toString().toStdString();
        const std::int64_t actorValue = *actor;
        if ((taskType != "TRAIN" && taskType != "PREDICT") ||
            (taskType == "PREDICT" && horizonHours.empty())) {
          response = errorResponse(ErrorCode::ValidationFailed,
                                   "ml task payload is invalid",
                                   "ML 任务请求内容不符合要求");
          response.end();
          return;
        }
        dispatchIdempotentBlocking(
            request, response, executor, idempotency,
            "a" + std::to_string(actorValue) + ":ml-task:" + taskType, false,
                         [&ops, actorValue, taskType, horizonHours](
                             core::application::IdempotencyLease &) {
                           const auto result = ops.startMlTask(
                               actorValue, taskType, horizonHours,
                               std::chrono::system_clock::now());
                           if (!result.ok())
                             return errorResponse(result.error, "", "");
                           return successResponse(
                               QJsonObject{{QStringLiteral("taskNo"),
                                               QString::fromStdString(result.value->taskNo)},
                                           {QStringLiteral("taskType"),
                                               QString::fromStdString(result.value->taskType)},
                                           {QStringLiteral("status"),
                                               QString::fromStdString(result.value->status)}},
                               202);
                         });
      });

  routes.route("/admin/ml-tasks/<string>").methods(crow::HTTPMethod::GET)(
      [&ops, &sessions, &executor](const crow::request &request,
                                  crow::response &response,
                                  std::string taskNo) {
        crow::response failure;
        if (!requireAdmin(request, sessions, failure)) {
          response = std::move(failure);
          response.end();
          return;
        }
        dispatchBlocking(request, response, executor,
                         [&ops, taskNo = std::move(taskNo)] {
          const auto result = ops.mlTask(taskNo);
          if (!result.ok())
            return errorResponse(result.error, "", "");
          return successResponse(QJsonObject{
            {QStringLiteral("taskNo"), QString::fromStdString(result.value->taskNo)},
            {QStringLiteral("taskType"), QString::fromStdString(result.value->taskType)},
            {QStringLiteral("status"), QString::fromStdString(result.value->status)},
            {QStringLiteral("modelVersion"), QString::fromStdString(result.value->modelVersion)}});
        });
      });
}

} // namespace ncs::server::controller
