#include "server/controller/order_payment_routes.h"
#include "server/controller/api_response.h"
#include "server/controller/async_response.h"
#include "server/controller/idempotent_response.h"
#include "server/controller/request_validation.h"
#include "server/controller/user_auth.h"
#include "server/middleware/authorization.h"
#include "core/application/admin_auth_service.h"
#include <QJsonArray>
#include <QJsonObject>
namespace ncs::server::controller {
namespace {
QJsonObject receiptJson(const core::application::SettlementReceipt& receipt)
{
    return {
        {QStringLiteral("flowNo"), QString::fromStdString(receipt.flowNo)},
        {QStringLiteral("orderNo"), QString::fromStdString(receipt.orderNo)},
        {QStringLiteral("stationName"), QString::fromStdString(receipt.stationName)},
        {QStringLiteral("chargerCode"), QString::fromStdString(receipt.chargerCode)},
        {QStringLiteral("startedAt"), QJsonValue(static_cast<qint64>(receipt.startedAt))},
        {QStringLiteral("endedAt"), QJsonValue(static_cast<qint64>(receipt.endedAt))},
        {QStringLiteral("durationSec"), QJsonValue(static_cast<qint64>(receipt.durationSec))},
        {QStringLiteral("energyMwh"), QJsonValue(static_cast<qint64>(receipt.energyMwh))},
        {QStringLiteral("electricityPriceCentPerKwh"), receipt.electricityPriceCentPerKwh},
        {QStringLiteral("servicePriceCentPerKwh"), receipt.servicePriceCentPerKwh},
        {QStringLiteral("amountCent"), QJsonValue(static_cast<qint64>(receipt.amountCent))},
        {QStringLiteral("paidCent"), QJsonValue(static_cast<qint64>(receipt.paidCent))},
        {QStringLiteral("debtAddedCent"), QJsonValue(static_cast<qint64>(receipt.debtAddedCent))},
        {QStringLiteral("balanceAfterCent"),
         QJsonValue(static_cast<qint64>(receipt.balanceAfterCent))},
        {QStringLiteral("debtAfterCent"), QJsonValue(static_cast<qint64>(receipt.debtAfterCent))},
        {QStringLiteral("settledAt"), receipt.settledAt ? QJsonValue(static_cast<qint64>(receipt.settledAt)) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("appealReason"), QString::fromStdString(receipt.appealReason)},
        {QStringLiteral("status"), receipt.status},
        {QStringLiteral("statusText"), QString::fromStdString(receipt.statusText)},
    };
}

}
void registerOrderPaymentRoutes(ApiRoutes& routes, core::application::ChargeFlowService& flows,
    core::application::SessionManager& sessions, core::application::BoundedExecutor& executor,
    core::application::IdempotencyService& idempotency) {
    for (const bool appeal : {false, true}) {
        routes.route(appeal ? "/user/orders/<string>/appeals" : "/user/orders/<string>/confirmation")
            .methods(crow::HTTPMethod::POST)(
                [&flows, &sessions, &executor, &idempotency, appeal]
                (const crow::request& request, crow::response& response, std::string orderNo) {
                    crow::response failure;
                    const auto userId = requireUserId(request, sessions, failure);
                    if (!userId) { response = std::move(failure); response.end(); return; }
                    const auto parsed = appeal ? parseJsonObject(request, {"reason"}, {"reason"})
                                               : parseJsonObject(request, {}, {});
                    QString reason;
                    if (parsed.object) reason = parsed.object->value("reason").toString().trimmed();
                    if (!parsed.object || (appeal && (!parsed.object->value("reason").isString() ||
                        reason.isEmpty() || reason.toUcs4().size() > 500 || reason.contains(QChar(0))))) {
                        response = errorResponse(core::domain::ErrorCode::ValidationFailed,
                            "invalid order action", "请填写 1～500 字申诉原因，或检查确认请求");
                        response.end(); return;
                    }
                    dispatchIdempotentBlocking(request, response, executor, idempotency,
                        "u" + std::to_string(*userId) + (appeal ? ":order-appeal:" : ":order-confirm:") + orderNo, true,
                        [&flows, userId = *userId, orderNo, appeal, reason = reason.toStdString()]
                        (core::application::IdempotencyLease&) {
                            const auto now = std::chrono::system_clock::now();
                            const auto result = appeal ? flows.appealOrder(userId, orderNo, reason, now)
                                                       : flows.confirmOrder(userId, orderNo, now);
                            return result.ok() ? successResponse(receiptJson(*result.value))
                                : errorResponse(result.error, "order action failed", "订单状态已变化或操作失败，请刷新订单后重试");
                        });
                });
    }
    routes.route("/admin/order-appeals").methods(crow::HTTPMethod::GET)(
        [&flows, &sessions, &executor](const crow::request& request, crow::response& response) {
            const auto auth = middleware::authorize(request, sessions,
                {core::application::TokenKind::Administrator},
                {core::application::Role::Operator, core::application::Role::Owner},
                std::chrono::system_clock::now());
            if (!auth.context) {
                response = errorResponse(auth.error, "administrator required", "需要管理员权限");
                response.end(); return;
            }
            const auto pagination = parsePagination(request, {"createdAt"}, {});
            if (!pagination) {
                response = errorResponse(core::domain::ErrorCode::ValidationFailed, "invalid paging", "分页参数无效");
                response.end(); return;
            }
            dispatchBlocking(request, response, executor, [&flows, page = pagination->page, size = pagination->pageSize] {
                const auto orders = flows.pendingAppeals();
                QJsonArray items;
                const auto begin = static_cast<std::size_t>(page - 1) * static_cast<std::size_t>(size);
                for (auto i = begin; i < orders.size() && i < begin + static_cast<std::size_t>(size); ++i) {
                    const auto& order = orders[i];
                    items.append(QJsonObject{{"orderNo", QString::fromStdString(order.orderNo)},
                        {"stationName", QString::fromStdString(order.stationName)},
                        {"chargerCode", QString::fromStdString(order.chargerCode)},
                        {"amountCent", static_cast<qint64>(order.amountCent)},
                        {"reason", QString::fromStdString(order.appealReason)},
                        {"appealAt", static_cast<qint64>(order.appealAt.value_or(0))}});
                }
                return successResponse(QJsonObject{{"items", items}, {"total", static_cast<int>(orders.size())},
                    {"page", page}, {"pageSize", size}});
            });
        });
    routes.route("/admin/order-appeals/<string>/approval").methods(crow::HTTPMethod::POST)(
        [&flows, &sessions, &executor, &idempotency]
        (const crow::request& request, crow::response& response, std::string orderNo) {
            const auto auth = middleware::authorize(request, sessions,
                {core::application::TokenKind::Administrator},
                {core::application::Role::Operator, core::application::Role::Owner},
                std::chrono::system_clock::now());
            if (!auth.context) {
                response = errorResponse(auth.error, "administrator required", "需要管理员权限");
                response.end(); return;
            }
            const auto adminId = core::application::AdminAuthService::principalId(auth.context->principalId);
            const auto parsed = parseJsonObject(request, {"confirmed"}, {"confirmed"});
            if (!adminId || !parsed.object || !parsed.object->value("confirmed").isBool() ||
                !parsed.object->value("confirmed").toBool()) {
                response = errorResponse(core::domain::ErrorCode::ValidationFailed, "confirmation required", "请确认审核通过并取消订单");
                response.end(); return;
            }
            dispatchIdempotentBlocking(request, response, executor, idempotency,
                "a" + std::to_string(*adminId) + ":appeal-approve:" + orderNo, true,
                [&flows, adminId = *adminId, orderNo](core::application::IdempotencyLease&) {
                    const auto result = flows.approveAppeal(adminId, orderNo, std::chrono::system_clock::now());
                    return result.ok() ? successResponse(receiptJson(*result.value))
                        : errorResponse(result.error, "appeal approval failed", "订单状态已变化或审核失败，请刷新后重试");
                });
        });


}
} // namespace ncs::server::controller
