#include "server/controller/wallet_routes.h"

#include "core/domain/error_code.h"
#include "server/controller/api_response.h"
#include "server/controller/idempotent_response.h"
#include "server/controller/request_validation.h"
#include "server/controller/user_auth.h"

#include <QJsonArray>
#include <QJsonObject>

namespace ncs::server::controller {
namespace {

QJsonObject overviewJson(const core::application::WalletOverview &overview) {
  return {
      {QStringLiteral("balanceCent"),
       QJsonValue(static_cast<qint64>(overview.balanceCent))},
      {QStringLiteral("debtCent"),
       QJsonValue(static_cast<qint64>(overview.debtCent))},
      {QStringLiteral("availableCent"),
       QJsonValue(static_cast<qint64>(overview.availableCent))},
      {QStringLiteral("version"),
       QJsonValue(static_cast<qint64>(overview.version))},
      {QStringLiteral("updatedAt"),
       QJsonValue(static_cast<qint64>(overview.updatedAt))},
  };
}

QJsonObject rechargeJson(const core::application::RechargeResult &result) {
  return {
      {QStringLiteral("rechargeNo"), QString::fromStdString(result.rechargeNo)},
      {QStringLiteral("requestedCent"),
       QJsonValue(static_cast<qint64>(result.requestedCent))},
      {QStringLiteral("debtPaidCent"),
       QJsonValue(static_cast<qint64>(result.debtPaidCent))},
      {QStringLiteral("balanceAddedCent"),
       QJsonValue(static_cast<qint64>(result.balanceAddedCent))},
      {QStringLiteral("balanceAfterCent"),
       QJsonValue(static_cast<qint64>(result.balanceAfterCent))},
      {QStringLiteral("debtAfterCent"),
       QJsonValue(static_cast<qint64>(result.debtAfterCent))},
      {QStringLiteral("completedAt"),
       QJsonValue(static_cast<qint64>(result.completedAt))},
  };
}

QJsonObject
transactionJson(const core::application::WalletTransactionView &view) {
  return {
      {QStringLiteral("transactionNo"),
       QString::fromStdString(view.transactionNo)},
      {QStringLiteral("type"), QString::fromStdString(view.type)},
      {QStringLiteral("amountCent"),
       QJsonValue(static_cast<qint64>(view.amountCent))},
      {QStringLiteral("balanceAfterCent"),
       QJsonValue(static_cast<qint64>(view.balanceAfterCent))},
      {QStringLiteral("debtAfterCent"),
       QJsonValue(static_cast<qint64>(view.debtAfterCent))},
      {QStringLiteral("relatedNo"), QString::fromStdString(view.relatedNo)},
      {QStringLiteral("createdAt"),
       QJsonValue(static_cast<qint64>(view.createdAt))},
  };
}

} // namespace

WalletRoutes::WalletRoutes(ApiRoutes &routes,
                           core::application::WalletService &wallet,
                           core::application::SessionManager &sessions,
                           core::application::BoundedExecutor &executor,
                           core::application::IdempotencyService &idempotency) {
  routes.route("/user/wallet")
      .methods(crow::HTTPMethod::GET)(
          [&wallet, &sessions](const crow::request &request) {
            crow::response failure;
            const auto userId = requireUserId(request, sessions, failure);
            if (!userId)
              return failure;
            return successResponse(overviewJson(wallet.overview(*userId)));
          });

  routes.route("/user/wallet/recharges")
      .methods(crow::HTTPMethod::POST)(
          [&wallet, &sessions, &executor, &idempotency](
              const crow::request &request, crow::response &response) {
            crow::response failure;
            const auto userId = requireUserId(request, sessions, failure);
            if (!userId) {
              response = std::move(failure);
              response.end();
              return;
            }
            const auto parsed =
                parseJsonObject(request, {"amountCent"}, {"amountCent"});
            if (!parsed.object ||
                !validIntegerField(
                    *parsed.object, "amountCent", 1,
                    core::application::WalletService::maximumRechargeCent)) {
              response =
                  errorResponse(core::domain::ErrorCode::ValidationFailed,
                                "recharge amount is outside the accepted range",
                                "充值金额需在 0.01 元到 10000 元之间");
              response.end();
              return;
            }
            const std::int64_t userIdValue = *userId;
            const std::int64_t amountCent = static_cast<std::int64_t>(
                parsed.object->value("amountCent").toDouble());
            dispatchIdempotentBlocking(
                request, response, executor, idempotency,
                "u" + std::to_string(userIdValue) + ":recharge", true,
                [&wallet, userIdValue,
                 amountCent](core::application::IdempotencyLease &) {
                  const auto result =
                      wallet.recharge(userIdValue, amountCent,
                                      std::chrono::system_clock::now());
                  if (!result.ok()) {
                    return errorResponse(
                        result.error,
                        "recharge amount is outside the accepted range",
                        "充值金额需在 0.01 元到 10000 元之间");
                  }
                  return successResponse(rechargeJson(*result.value));
                });
          });

  routes.route("/user/wallet/transactions")
      .methods(crow::HTTPMethod::GET)([&wallet, &sessions](
                                          const crow::request &request) {
        crow::response failure;
        const auto userId = requireUserId(request, sessions, failure);
        if (!userId)
          return failure;
        const auto pagination =
            parsePagination(request, {}, {"type", "fromAt", "toAt"});
        if (!pagination) {
          return errorResponse(core::domain::ErrorCode::ValidationFailed,
                               "unsupported paging or filter parameter",
                               "分页或过滤参数不符合要求");
        }
        const auto fromAt =
            parseIntegerFilter(*pagination, "fromAt", 0, 4102444800LL);
        const auto toAt =
            parseIntegerFilter(*pagination, "toAt", 0, 4102444800LL);
        if (!fromAt || !toAt ||
            (fromAt->has_value() && toAt->has_value() && **fromAt > **toAt)) {
          return errorResponse(core::domain::ErrorCode::ValidationFailed,
                               "transaction time filter is invalid",
                               "流水时间范围不符合要求");
        }
        const auto result = wallet.transactions(
            *userId,
            pagination->filters.count("type") ? pagination->filters.at("type")
                                              : std::string(),
            fromAt->value_or(0), toAt->value_or(0), pagination->page,
            pagination->pageSize);
        if (!result.ok()) {
          return errorResponse(result.error,
                               "unsupported transaction type filter",
                               "流水类型过滤不符合要求");
        }
        QJsonArray items;
        for (const auto &view : result.value->items)
          items.append(transactionJson(view));
        return successResponse(QJsonObject{
            {QStringLiteral("items"), items},
            {QStringLiteral("total"), result.value->total},
            {QStringLiteral("page"), result.value->page},
            {QStringLiteral("pageSize"), result.value->pageSize},
        });
      });
}

} // namespace ncs::server::controller
