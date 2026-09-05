#include "server/controller/station_routes.h"

#include "core/domain/error_code.h"
#include "server/controller/api_response.h"
#include "server/controller/async_response.h"
#include "server/controller/request_validation.h"
#include "server/controller/user_auth.h"

#include <QJsonArray>
#include <QJsonObject>

namespace ncs::server::controller {
namespace {

std::string filterText(const Pagination &pagination, const char *key) {
  const auto found = pagination.filters.find(key);
  return found == pagination.filters.end() ? std::string() : found->second;
}

QJsonObject stationJson(const core::application::StationSummary &summary) {
  return {
      {QStringLiteral("id"), QJsonValue(static_cast<qint64>(summary.id))},
      {QStringLiteral("code"), QString::fromStdString(summary.code)},
      {QStringLiteral("name"), QString::fromStdString(summary.name)},
      {QStringLiteral("address"), QString::fromStdString(summary.address)},
      {QStringLiteral("adcode"), QString::fromStdString(summary.adcode)},
      {QStringLiteral("latitudeE6"),
       QJsonValue(static_cast<qint64>(summary.latitudeE6))},
      {QStringLiteral("longitudeE6"),
       QJsonValue(static_cast<qint64>(summary.longitudeE6))},
      {QStringLiteral("electricityPriceCentPerKwh"),
       summary.electricityPriceCentPerKwh},
      {QStringLiteral("servicePriceCentPerKwh"),
       summary.servicePriceCentPerKwh},
      {QStringLiteral("totalPriceCentPerKwh"), summary.totalPriceCentPerKwh},
      {QStringLiteral("idleCount"), summary.idleCount},
      {QStringLiteral("operationalCount"), summary.operationalCount},
      {QStringLiteral("totalCount"), summary.totalCount},
      {QStringLiteral("distanceMeter"),
       QJsonValue(static_cast<qint64>(summary.distanceMeter))},
  };
}

crow::response chargerListResponse(const core::application::ChargerPage &page) {
  QJsonArray items;
  for (const auto &charger : page.items) {
    items.append(QJsonObject{
        {QStringLiteral("id"), QJsonValue(static_cast<qint64>(charger.id))},
        {QStringLiteral("code"), QString::fromStdString(charger.code)},
        {QStringLiteral("chargerType"),
         charger.type == core::application::ChargerType::DcFast ? 1 : 0},
        {QStringLiteral("chargerTypeText"),
         QString::fromStdString(
             core::application::chargerTypeName(charger.type))},
        {QStringLiteral("powerWatt"),
         QJsonValue(static_cast<qint64>(charger.powerWatt))},
        {QStringLiteral("connectorStandard"),
         QString::fromStdString(charger.connectorStandard)},
        {QStringLiteral("status"), charger.status},
        {QStringLiteral("statusText"),
         QString::fromStdString(charger.statusText)},
        {QStringLiteral("totalCount"),
         QJsonValue(static_cast<qint64>(charger.totalCount))},
    });
  }
  return successResponse(QJsonObject{
      {QStringLiteral("items"), items},
      {QStringLiteral("total"), page.total},
      {QStringLiteral("page"), page.page},
      {QStringLiteral("pageSize"), page.pageSize},
  });
}

} // namespace

StationRoutes::StationRoutes(ApiRoutes &routes,
                             core::application::StationService &stations,
                             core::application::SessionManager &sessions,
                             core::application::BoundedExecutor &executor) {
  routes.route("/user/stations")
      .methods(crow::HTTPMethod::GET)([&stations, &sessions,
                                       &executor](const crow::request &request,
                                                  crow::response &response) {
        crow::response failure;
        if (!requireUserId(request, sessions, failure)) {
          response = std::move(failure);
          response.end();
          return;
        }
        const auto pagination = parsePagination(
            request, {},
            {"latitudeE6", "longitudeE6", "keyword", "chargerType"});
        if (!pagination) {
          response = errorResponse(core::domain::ErrorCode::ValidationFailed,
                                   "unsupported paging or filter parameter",
                                   "分页或过滤参数不符合要求");
          response.end();
          return;
        }
        const auto chargerType =
            parseIntegerFilter(*pagination, "chargerType", 0, 1);
        const auto latitude =
            parseIntegerFilter(*pagination, "latitudeE6", -90000000, 90000000);
        const auto longitude = parseIntegerFilter(*pagination, "longitudeE6",
                                                  -180000000, 180000000);
        if (!chargerType || !latitude || !longitude ||
            (latitude->has_value() != longitude->has_value())) {
          response = errorResponse(core::domain::ErrorCode::ValidationFailed,
                                   "invalid location or charger type",
                                   "位置或充电类型不符合要求");
          response.end();
          return;
        }
        dispatchBlocking(
            request, response, executor,
            [&stations, pagination = *pagination, chargerType, latitude,
             longitude]() {
              const auto result = stations.nearbyStations(
                  *latitude, *longitude, filterText(pagination, "keyword"),
                  chargerType->has_value()
                      ? std::optional<core::application::ChargerType>(
                            static_cast<core::application::ChargerType>(
                                **chargerType))
                      : std::nullopt,
                  pagination.page, pagination.pageSize,
                  std::chrono::system_clock::now());
              QJsonArray items;
              for (const auto &summary : result.items)
                items.append(stationJson(summary));
              return successResponse(QJsonObject{
                  {QStringLiteral("items"), items},
                  {QStringLiteral("total"), result.total},
                  {QStringLiteral("page"), result.page},
                  {QStringLiteral("pageSize"), result.pageSize},
                  {QStringLiteral("locationFallback"), result.locationFallback},
              });
            });
      });

  routes.route("/user/stations/<int>")
      .methods(crow::HTTPMethod::GET)([&stations,
                                       &sessions](const crow::request &request,
                                                  std::int64_t stationId) {
        crow::response failure;
        if (!requireUserId(request, sessions, failure))
          return failure;
        const auto result =
            stations.stationDetail(stationId, std::chrono::system_clock::now());
        if (!result.ok()) {
          return errorResponse(result.error, "station not found",
                               "未找到相关数据");
        }
        QJsonArray supportedTypes;
        for (const auto type : result.value->supportedTypes) {
          supportedTypes.append(
              type == core::application::ChargerType::DcFast ? 1 : 0);
        }
        QJsonObject data = stationJson(result.value->summary);
        data.insert(QStringLiteral("businessHours"),
                    QString::fromStdString(result.value->businessHours));
        data.insert(QStringLiteral("supportedTypes"), supportedTypes);
        return successResponse(std::move(data));
      });

  routes.route("/user/stations/<int>/chargers")
      .methods(crow::HTTPMethod::GET)([&stations,
                                       &sessions](const crow::request &request,
                                                  std::int64_t stationId) {
        crow::response failure;
        if (!requireUserId(request, sessions, failure))
          return failure;
        const auto pagination =
            parsePagination(request, {}, {"chargerType", "status"});
        if (!pagination) {
          return errorResponse(core::domain::ErrorCode::ValidationFailed,
                               "unsupported paging or filter parameter",
                               "分页或过滤参数不符合要求");
        }
        const auto chargerType =
            parseIntegerFilter(*pagination, "chargerType", 0, 1);
        const auto status = parseIntegerFilter(*pagination, "status", 0, 3);
        if (!chargerType || !status) {
          return errorResponse(core::domain::ErrorCode::ValidationFailed,
                               "unsupported charger type",
                               "充电类型不符合要求");
        }
        const auto result = stations.stationChargers(
            stationId,
            chargerType->has_value()
                ? std::optional<core::application::ChargerType>(
                      static_cast<core::application::ChargerType>(
                          **chargerType))
                : std::nullopt,
            status->has_value() ? std::optional<int>(static_cast<int>(**status))
                                : std::nullopt,
            pagination->page, pagination->pageSize);
        if (!result.ok())
          return errorResponse(result.error, "invalid request",
                               "请求内容不符合要求");
        return chargerListResponse(*result.value);
      });

  routes.route("/user/stations/<int>/quote")
      .methods(crow::HTTPMethod::GET)([&stations,
                                       &sessions](const crow::request &request,
                                                  std::int64_t stationId) {
        crow::response failure;
        if (!requireUserId(request, sessions, failure))
          return failure;
        const auto pagination = parsePagination(request, {}, {"chargerType"});
        if (!pagination) {
          return errorResponse(core::domain::ErrorCode::ValidationFailed,
                               "unsupported paging or filter parameter",
                               "分页或过滤参数不符合要求");
        }
        const auto chargerType =
            parseIntegerFilter(*pagination, "chargerType", 0, 1);
        if (!chargerType || !chargerType->has_value()) {
          return errorResponse(core::domain::ErrorCode::ValidationFailed,
                               "chargerType is required", "缺少有效的充电类型");
        }
        const auto result = stations.stationQuote(
            stationId,
            static_cast<core::application::ChargerType>(**chargerType),
            std::chrono::system_clock::now());
        if (!result.ok())
          return errorResponse(result.error, "station not found",
                               "未找到相关数据");
        return successResponse(QJsonObject{
            {QStringLiteral("electricityPriceCentPerKwh"),
             result.value->electricityPriceCentPerKwh},
            {QStringLiteral("baseServicePriceCentPerKwh"),
             result.value->baseServicePriceCentPerKwh},
            {QStringLiteral("queueAdjustmentBp"),
             result.value->queueAdjustmentBp},
            {QStringLiteral("mlAdjustmentBp"), result.value->mlAdjustmentBp},
            {QStringLiteral("finalServicePriceCentPerKwh"),
             result.value->finalServicePriceCentPerKwh},
            {QStringLiteral("totalPriceCentPerKwh"),
             result.value->totalPriceCentPerKwh},
            {QStringLiteral("calculatedAt"),
             QJsonValue(static_cast<qint64>(result.value->calculatedAt))},
        });
      });
}

} // namespace ncs::server::controller
