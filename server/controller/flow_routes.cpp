#include "server/controller/flow_routes.h"

#include "core/domain/error_code.h"
#include "server/controller/api_response.h"
#include "server/controller/idempotent_response.h"
#include "server/controller/request_validation.h"
#include "server/controller/user_auth.h"

#include <QJsonArray>
#include <QJsonObject>

namespace ncs::server::controller {
namespace {

QJsonObject quoteJson(const core::application::FlowQuoteView &quote) {
  return {
      {QStringLiteral("quoteNo"), QString::fromStdString(quote.quoteNo)},
      {QStringLiteral("chargerId"),
       QJsonValue(static_cast<qint64>(quote.chargerId))},
      {QStringLiteral("chargerCode"),
       QString::fromStdString(quote.chargerCode)},
      {QStringLiteral("electricityPriceCentPerKwh"),
       quote.electricityPriceCentPerKwh},
      {QStringLiteral("baseServicePriceCentPerKwh"),
       quote.baseServicePriceCentPerKwh},
      {QStringLiteral("queueAdjustmentBp"), quote.queueAdjustmentBp},
      {QStringLiteral("mlAdjustmentBp"), quote.mlAdjustmentBp},
      {QStringLiteral("finalServicePriceCentPerKwh"),
       quote.finalServicePriceCentPerKwh},
      {QStringLiteral("totalPriceCentPerKwh"), quote.totalPriceCentPerKwh},
      {QStringLiteral("expiresAt"),
       QJsonValue(static_cast<qint64>(quote.expiresAt))},
  };
}

QJsonObject flowJson(const core::application::FlowView &view) {
  QJsonObject data{
      {QStringLiteral("flowNo"), QString::fromStdString(view.flowNo)},
      {QStringLiteral("stationId"),
       QJsonValue(static_cast<qint64>(view.stationId))},
      {QStringLiteral("chargerType"),
       view.chargerType == core::application::ChargerType::DcFast ? 1 : 0},
      {QStringLiteral("status"), view.status},
      {QStringLiteral("statusText"), QString::fromStdString(view.statusText)},
      {QStringLiteral("version"),
       QJsonValue(static_cast<qint64>(view.version))},
  };
  if (view.chargerId)
    data.insert(QStringLiteral("chargerId"),
                QJsonValue(static_cast<qint64>(*view.chargerId)));
  else
    data.insert(QStringLiteral("chargerId"), QJsonValue(QJsonValue::Null));
  if (view.chargerCode)
    data.insert(QStringLiteral("chargerCode"),
                QString::fromStdString(*view.chargerCode));
  else
    data.insert(QStringLiteral("chargerCode"), QJsonValue(QJsonValue::Null));
  if (view.queuePosition)
    data.insert(QStringLiteral("queuePosition"), *view.queuePosition);
  else
    data.insert(QStringLiteral("queuePosition"), QJsonValue(QJsonValue::Null));
  if (view.quote)
    data.insert(QStringLiteral("quote"), quoteJson(*view.quote));
  else
    data.insert(QStringLiteral("quote"), QJsonValue(QJsonValue::Null));
  if (view.reservedUntil)
    data.insert(QStringLiteral("reservedUntil"),
                QJsonValue(static_cast<qint64>(*view.reservedUntil)));
  else
    data.insert(QStringLiteral("reservedUntil"), QJsonValue(QJsonValue::Null));
  if (view.startedAt)
    data.insert(QStringLiteral("startedAt"),
                QJsonValue(static_cast<qint64>(*view.startedAt)));
  else
    data.insert(QStringLiteral("startedAt"), QJsonValue(QJsonValue::Null));
  return data;
}

QJsonObject receiptJson(const core::application::SettlementReceipt &receipt) {
  return {
      {QStringLiteral("flowNo"), QString::fromStdString(receipt.flowNo)},
      {QStringLiteral("orderNo"), QString::fromStdString(receipt.orderNo)},
      {QStringLiteral("stationName"),
       QString::fromStdString(receipt.stationName)},
      {QStringLiteral("chargerCode"),
       QString::fromStdString(receipt.chargerCode)},
      {QStringLiteral("startedAt"),
       QJsonValue(static_cast<qint64>(receipt.startedAt))},
      {QStringLiteral("endedAt"),
       QJsonValue(static_cast<qint64>(receipt.endedAt))},
      {QStringLiteral("durationSec"),
       QJsonValue(static_cast<qint64>(receipt.durationSec))},
      {QStringLiteral("energyMwh"),
       QJsonValue(static_cast<qint64>(receipt.energyMwh))},
      {QStringLiteral("electricityPriceCentPerKwh"),
       receipt.electricityPriceCentPerKwh},
      {QStringLiteral("servicePriceCentPerKwh"),
       receipt.servicePriceCentPerKwh},
      {QStringLiteral("amountCent"),
       QJsonValue(static_cast<qint64>(receipt.amountCent))},
      {QStringLiteral("paidCent"),
       QJsonValue(static_cast<qint64>(receipt.paidCent))},
      {QStringLiteral("debtAddedCent"),
       QJsonValue(static_cast<qint64>(receipt.debtAddedCent))},
      {QStringLiteral("balanceAfterCent"),
       QJsonValue(static_cast<qint64>(receipt.balanceAfterCent))},
      {QStringLiteral("debtAfterCent"),
       QJsonValue(static_cast<qint64>(receipt.debtAfterCent))},
      {QStringLiteral("settledAt"),
       QJsonValue(static_cast<qint64>(receipt.settledAt))},
      {QStringLiteral("status"), receipt.status},
      {QStringLiteral("statusText"),
       QString::fromStdString(receipt.statusText)},
  };
}

} // namespace

FlowRoutes::FlowRoutes(ApiRoutes &routes,
                       core::application::ChargeFlowService &flows,
                       core::application::SessionManager &sessions,
                       core::application::BoundedExecutor &executor,
                       core::application::IdempotencyService &idempotency) {
  routes.route("/user/flows")
      .methods(
          crow::HTTPMethod::POST)([&flows, &sessions, &executor,
                                   &idempotency](const crow::request &request,
                                                 crow::response &response) {
        crow::response failure;
        const auto userId = requireUserId(request, sessions, failure);
        if (!userId) {
          response = std::move(failure);
          response.end();
          return;
        }
        const auto parsed = parseJsonObject(
            request, {"stationId", "chargerType", "preferredChargerId"},
            {"stationId", "chargerType"});
        if (!parsed.object ||
            !validIntegerField(*parsed.object, "stationId", 1,
                               9007199254740991LL) ||
            !validIntegerField(*parsed.object, "chargerType", 0, 1)) {
          response =
              errorResponse(core::domain::ErrorCode::ValidationFailed,
                            "flow request is invalid: " + parsed.diagnostic,
                            "充电请求内容不符合要求");
          response.end();
          return;
        }
        std::optional<std::int64_t> preferredChargerId;
        const QJsonValue preferred = parsed.object->value("preferredChargerId");
        if (!preferred.isUndefined() && !preferred.isNull()) {
          if (!validIntegerField(*parsed.object, "preferredChargerId", 1,
                                 9007199254740991LL)) {
            response = errorResponse(core::domain::ErrorCode::ValidationFailed,
                                     "flow request is invalid",
                                     "充电请求内容不符合要求");
            response.end();
            return;
          }
          preferredChargerId = static_cast<std::int64_t>(preferred.toDouble());
        }
        const std::int64_t userIdValue = *userId;
        const std::int64_t stationId = static_cast<std::int64_t>(
            parsed.object->value("stationId").toDouble());
        const int chargerType = parsed.object->value("chargerType").toInt();
        dispatchIdempotentBlocking(
            request, response, executor, idempotency,
            "u" + std::to_string(userIdValue) + ":flow-create", false,
            [&flows, userIdValue, stationId, chargerType,
             preferredChargerId](core::application::IdempotencyLease &) {
              const auto result = flows.createFlow(
                  userIdValue, stationId, chargerType, preferredChargerId,
                  std::chrono::system_clock::now());
              if (!result.ok())
                return errorResponse(result.error, "", "");
              return successResponse(flowJson(*result.value));
            });
      });

  routes.route("/user/flows/active")
      .methods(crow::HTTPMethod::GET)(
          [&flows, &sessions](const crow::request &request) {
            crow::response failure;
            const auto userId = requireUserId(request, sessions, failure);
            if (!userId)
              return failure;
            const auto view =
                flows.activeFlow(*userId, std::chrono::system_clock::now());
            if (!view.hasActiveFlow) {
              return successResponse(QJsonObject{
                  {QStringLiteral("hasActiveFlow"), false},
                  {QStringLiteral("flow"), QJsonValue(QJsonValue::Null)},
              });
            }
            return successResponse(QJsonObject{
                {QStringLiteral("hasActiveFlow"), true},
                {QStringLiteral("flow"), flowJson(*view.flow)},
            });
          });

  routes.route("/user/flows/<string>")
      .methods(crow::HTTPMethod::GET)([&flows,
                                       &sessions](const crow::request &request,
                                                  std::string flowNo) {
        crow::response failure;
        const auto userId = requireUserId(request, sessions, failure);
        if (!userId)
          return failure;
        const auto result =
            flows.flowView(*userId, flowNo, std::chrono::system_clock::now());
        if (!result.ok())
          return errorResponse(result.error, "", "");
        return successResponse(flowJson(*result.value));
      });

  routes.route("/user/flows/<string>/quote-confirmations")
      .methods(
          crow::HTTPMethod::POST)([&flows, &sessions, &executor,
                                   &idempotency](const crow::request &request,
                                                 crow::response &response,
                                                 std::string flowNo) {
        crow::response failure;
        const auto userId = requireUserId(request, sessions, failure);
        if (!userId) {
          response = std::move(failure);
          response.end();
          return;
        }
        const auto parsed = parseJsonObject(request, {"quoteNo", "flowVersion"},
                                            {"quoteNo", "flowVersion"});
        if (!parsed.object || !parsed.object->value("quoteNo").isString() ||
            !validIntegerField(*parsed.object, "flowVersion", 1,
                               9007199254740991LL)) {
          response = errorResponse(core::domain::ErrorCode::ValidationFailed,
                                   "quote confirmation is invalid",
                                   "报价确认内容不符合要求");
          response.end();
          return;
        }
        const std::int64_t userIdValue = *userId;
        const std::string quoteNo =
            parsed.object->value("quoteNo").toString().toStdString();
        const std::int64_t flowVersion = static_cast<std::int64_t>(
            parsed.object->value("flowVersion").toDouble());
        dispatchIdempotentBlocking(
            request, response, executor, idempotency,
            "u" + std::to_string(userIdValue) + ":flow-confirm:" + flowNo,
            false,
            [&flows, userIdValue, flowNo, quoteNo,
             flowVersion](core::application::IdempotencyLease &) {
              const auto result =
                  flows.confirmQuote(userIdValue, flowNo, quoteNo, flowVersion,
                                     std::chrono::system_clock::now());
              if (!result.ok())
                return errorResponse(result.error, "", "");
              return successResponse(QJsonObject{
                  {QStringLiteral("flowNo"),
                   QString::fromStdString(result.value->flowNo)},
                  {QStringLiteral("orderNo"),
                   QString::fromStdString(result.value->orderNo)},
                  {QStringLiteral("status"), result.value->status},
                  {QStringLiteral("statusText"),
                   QString::fromStdString(core::application::flowStatusText(
                       result.value->status))},
                  {QStringLiteral("chargerId"),
                   QJsonValue(static_cast<qint64>(result.value->chargerId))},
                  {QStringLiteral("chargerCode"),
                   QString::fromStdString(result.value->chargerCode)},
                  {QStringLiteral("reservedUntil"),
                   QJsonValue(
                       static_cast<qint64>(result.value->reservedUntil))},
                  {QStringLiteral("version"),
                   QJsonValue(static_cast<qint64>(result.value->version))},
              });
            });
      });

  routes.route("/user/flows/<string>/cancellations")
      .methods(
          crow::HTTPMethod::POST)([&flows, &sessions, &executor,
                                   &idempotency](const crow::request &request,
                                                 crow::response &response,
                                                 std::string flowNo) {
        crow::response failure;
        const auto userId = requireUserId(request, sessions, failure);
        if (!userId) {
          response = std::move(failure);
          response.end();
          return;
        }
        const auto parsed =
            parseJsonObject(request, {"reasonCode", "flowVersion"},
                            {"reasonCode", "flowVersion"});
        if (!parsed.object ||
            !validStringField(*parsed.object, "reasonCode", 1, 32) ||
            !validIntegerField(*parsed.object, "flowVersion", 1,
                               9007199254740991LL)) {
          response = errorResponse(core::domain::ErrorCode::ValidationFailed,
                                   "cancellation is invalid",
                                   "取消请求内容不符合要求");
          response.end();
          return;
        }
        const std::int64_t userIdValue = *userId;
        const std::string reasonCode =
            parsed.object->value("reasonCode").toString().toStdString();
        const std::int64_t flowVersion = static_cast<std::int64_t>(
            parsed.object->value("flowVersion").toDouble());
        dispatchIdempotentBlocking(
            request, response, executor, idempotency,
            "u" + std::to_string(userIdValue) + ":flow-cancel:" + flowNo, false,
            [&flows, userIdValue, flowNo, reasonCode,
             flowVersion](core::application::IdempotencyLease &) {
              const auto result =
                  flows.cancel(userIdValue, flowNo, reasonCode, flowVersion,
                               std::chrono::system_clock::now());
              if (!result.ok())
                return errorResponse(result.error, "", "");
              return successResponse(flowJson(*result.value));
            });
      });

  routes.route("/user/flows/<string>/start")
      .methods(
          crow::HTTPMethod::POST)([&flows, &sessions, &executor,
                                   &idempotency](const crow::request &request,
                                                 crow::response &response,
                                                 std::string flowNo) {
        crow::response failure;
        const auto userId = requireUserId(request, sessions, failure);
        if (!userId) {
          response = std::move(failure);
          response.end();
          return;
        }
        const auto parsed = parseJsonObject(
            request, {"flowVersion", "targetAmountCent", "balanceFloorCent"},
            {"flowVersion"});
        if (!parsed.object || !validIntegerField(*parsed.object, "flowVersion",
                                                 1, 9007199254740991LL)) {
          response =
              errorResponse(core::domain::ErrorCode::ValidationFailed,
                            "start request is invalid: " + parsed.diagnostic,
                            "开始充电请求内容不符合要求");
          response.end();
          return;
        }
        std::optional<std::int64_t> targetAmountCent;
        const QJsonValue target = parsed.object->value("targetAmountCent");
        if (!target.isUndefined() && !target.isNull()) {
          if (!validIntegerField(*parsed.object, "targetAmountCent", 1,
                                 1000000000LL)) {
            response = errorResponse(core::domain::ErrorCode::ValidationFailed,
                                     "start request is invalid",
                                     "开始充电请求内容不符合要求");
            response.end();
            return;
          }
          targetAmountCent = static_cast<std::int64_t>(target.toDouble());
        }
        std::optional<std::int64_t> balanceFloorCent;
        const QJsonValue floor = parsed.object->value("balanceFloorCent");
        if (!floor.isUndefined() && !floor.isNull()) {
          if (!validIntegerField(*parsed.object, "balanceFloorCent", 0,
                                 1000000000LL)) {
            response = errorResponse(core::domain::ErrorCode::ValidationFailed,
                                     "start request is invalid",
                                     "开始充电请求内容不符合要求");
            response.end();
            return;
          }
          balanceFloorCent = static_cast<std::int64_t>(floor.toDouble());
        }
        const std::int64_t userIdValue = *userId;
        const std::int64_t flowVersion = static_cast<std::int64_t>(
            parsed.object->value("flowVersion").toDouble());
        dispatchIdempotentBlocking(
            request, response, executor, idempotency,
            "u" + std::to_string(userIdValue) + ":flow-start:" + flowNo, false,
            [&flows, userIdValue, flowNo, flowVersion, targetAmountCent,
             balanceFloorCent](core::application::IdempotencyLease &) {
              const auto result = flows.start(
                  userIdValue, flowNo, flowVersion, targetAmountCent,
                  balanceFloorCent, std::chrono::system_clock::now());
              if (!result.ok())
                return errorResponse(result.error, "", "");
              return successResponse(QJsonObject{
                  {QStringLiteral("flowNo"),
                   QString::fromStdString(result.value->flowNo)},
                  {QStringLiteral("orderNo"),
                   QString::fromStdString(result.value->orderNo)},
                  {QStringLiteral("status"), result.value->status},
                  {QStringLiteral("statusText"), QStringLiteral("充电中")},
                  {QStringLiteral("startedAt"),
                   QJsonValue(static_cast<qint64>(result.value->startedAt))},
                  {QStringLiteral("powerWatt"),
                   QJsonValue(static_cast<qint64>(result.value->powerWatt))},
                  {QStringLiteral("timeScale"), result.value->timeScale},
                  {QStringLiteral("version"),
                   QJsonValue(static_cast<qint64>(result.value->version))},
              });
            });
      });

  routes.route("/user/flows/<string>/progress")
      .methods(crow::HTTPMethod::GET)([&flows,
                                       &sessions](const crow::request &request,
                                                  std::string flowNo) {
        crow::response failure;
        const auto userId = requireUserId(request, sessions, failure);
        if (!userId)
          return failure;
        const auto result =
            flows.progress(*userId, flowNo, std::chrono::system_clock::now());
        if (!result.ok())
          return errorResponse(result.error, "", "");
        return successResponse(QJsonObject{
            {QStringLiteral("flowNo"),
             QString::fromStdString(result.value->flowNo)},
            {QStringLiteral("orderNo"),
             QString::fromStdString(result.value->orderNo)},
            {QStringLiteral("status"), result.value->status},
            {QStringLiteral("statusText"),
             QString::fromStdString(result.value->statusText)},
            {QStringLiteral("durationSec"),
             QJsonValue(static_cast<qint64>(result.value->durationSec))},
            {QStringLiteral("energyMwh"),
             QJsonValue(static_cast<qint64>(result.value->energyMwh))},
            {QStringLiteral("amountCent"),
             QJsonValue(static_cast<qint64>(result.value->amountCent))},
            {QStringLiteral("powerWatt"),
             QJsonValue(static_cast<qint64>(result.value->powerWatt))},
            {QStringLiteral("simulatedSoc"), result.value->simulatedSoc},
            {QStringLiteral("calculatedAt"),
             QJsonValue(static_cast<qint64>(result.value->calculatedAt))},
        });
      });

  routes.route("/user/flows/<string>/settlements")
      .methods(
          crow::HTTPMethod::POST)([&flows, &sessions, &executor,
                                   &idempotency](const crow::request &request,
                                                 crow::response &response,
                                                 std::string flowNo) {
        crow::response failure;
        const auto userId = requireUserId(request, sessions, failure);
        if (!userId) {
          response = std::move(failure);
          response.end();
          return;
        }
        const auto parsed =
            parseJsonObject(request, {"flowVersion", "reasonCode"},
                            {"flowVersion", "reasonCode"});
        if (!parsed.object ||
            !validStringField(*parsed.object, "reasonCode", 1, 32) ||
            !validIntegerField(*parsed.object, "flowVersion", 1,
                               9007199254740991LL)) {
          response = errorResponse(core::domain::ErrorCode::ValidationFailed,
                                   "settlement request is invalid",
                                   "结算请求内容不符合要求");
          response.end();
          return;
        }
        const std::int64_t userIdValue = *userId;
        const std::string reasonCode =
            parsed.object->value("reasonCode").toString().toStdString();
        const std::int64_t flowVersion = static_cast<std::int64_t>(
            parsed.object->value("flowVersion").toDouble());
        dispatchIdempotentBlocking(
            request, response, executor, idempotency,
            "u" + std::to_string(userIdValue) + ":flow-settle:" + flowNo, true,
            [&flows, userIdValue, flowNo, reasonCode,
             flowVersion](core::application::IdempotencyLease &) {
              const auto result =
                  flows.settle(userIdValue, flowNo, flowVersion, reasonCode,
                               std::chrono::system_clock::now());
              if (!result.ok())
                return errorResponse(result.error, "", "");
              return successResponse(receiptJson(*result.value));
            });
      });

  routes.route("/user/orders")
      .methods(crow::HTTPMethod::GET)([&flows, &sessions](
                                          const crow::request &request) {
        crow::response failure;
        const auto userId = requireUserId(request, sessions, failure);
        if (!userId)
          return failure;
        const auto pagination = parsePagination(
            request, {"createdAt", "-createdAt"}, {"status", "fromAt", "toAt"});
        if (!pagination) {
          return errorResponse(core::domain::ErrorCode::ValidationFailed,
                               "unsupported paging or filter parameter",
                               "分页或过滤参数不符合要求");
        }
        const auto status = parseIntegerFilter(*pagination, "status", 60, 90);
        const auto fromAt =
            parseIntegerFilter(*pagination, "fromAt", 0, 4102444800LL);
        const auto toAt =
            parseIntegerFilter(*pagination, "toAt", 0, 4102444800LL);
        if (!status || !fromAt || !toAt ||
            (fromAt->has_value() && toAt->has_value() && **fromAt > **toAt)) {
          return errorResponse(core::domain::ErrorCode::ValidationFailed,
                               "order filters are invalid",
                               "订单过滤参数不符合要求");
        }
        const auto result = flows.orders(
            *userId,
            status->has_value() ? std::optional<int>(static_cast<int>(**status))
                                : std::nullopt,
            fromAt->value_or(0), toAt->value_or(0), pagination->sort,
            pagination->page, pagination->pageSize);
        if (!result.ok()) {
          return errorResponse(result.error, "unsupported order filter",
                               "订单过滤参数不符合要求");
        }
        QJsonArray items;
        for (const auto &order : result.value->items) {
          QJsonObject item{
              {QStringLiteral("orderNo"),
               QString::fromStdString(order.orderNo)},
              {QStringLiteral("flowNo"), QString::fromStdString(order.flowNo)},
              {QStringLiteral("stationName"),
               QString::fromStdString(order.stationName)},
              {QStringLiteral("chargerCode"),
               QString::fromStdString(order.chargerCode)},
              {QStringLiteral("status"), order.status},
              {QStringLiteral("statusText"),
               QString::fromStdString(order.statusText)},
              {QStringLiteral("energyMwh"),
               QJsonValue(static_cast<qint64>(order.energyMwh))},
              {QStringLiteral("amountCent"),
               QJsonValue(static_cast<qint64>(order.amountCent))},
          };
          if (order.startedAt)
            item.insert(QStringLiteral("startedAt"),
                        QJsonValue(static_cast<qint64>(*order.startedAt)));
          else
            item.insert(QStringLiteral("startedAt"),
                        QJsonValue(QJsonValue::Null));
          if (order.endedAt)
            item.insert(QStringLiteral("endedAt"),
                        QJsonValue(static_cast<qint64>(*order.endedAt)));
          else
            item.insert(QStringLiteral("endedAt"),
                        QJsonValue(QJsonValue::Null));
          items.append(std::move(item));
        }
        return successResponse(QJsonObject{
            {QStringLiteral("items"), items},
            {QStringLiteral("total"), result.value->total},
            {QStringLiteral("page"), result.value->page},
            {QStringLiteral("pageSize"), result.value->pageSize},
        });
      });

  routes.route("/user/orders/<string>")
      .methods(crow::HTTPMethod::GET)(
          [&flows, &sessions](const crow::request &request,
                              std::string orderNo) {
            crow::response failure;
            const auto userId = requireUserId(request, sessions, failure);
            if (!userId)
              return failure;
            const auto result = flows.receipt(*userId, orderNo);
            if (!result.ok())
              return errorResponse(result.error, "", "");
            return successResponse(receiptJson(*result.value));
          });
}

} // namespace ncs::server::controller
