#include "server/controller/order_review_routes.h"

#include "server/controller/api_response.h"
#include "server/controller/async_response.h"
#include "server/controller/idempotent_response.h"
#include "server/controller/request_validation.h"
#include "server/controller/user_auth.h"

#include <QJsonArray>
#include <QJsonObject>

#include <cstddef>

namespace ncs::server::controller
{
namespace
{
// UC-U-12 场站评论墙单次返回的最新评价上限。
constexpr std::size_t kStationReviewWallLimit = 200;

QJsonObject reviewJson(const core::application::OrderReview& review)
{
    return {{"orderNo", QString::fromStdString(review.orderNo)},
            {"rating", review.rating},
            {"content", QString::fromStdString(review.content)},
            {"createdAt", QJsonValue(static_cast<qint64>(review.createdAt))}};
}
crow::response reviewError(core::domain::ErrorCode error)
{
    using core::domain::ErrorCode;
    switch (error)
    {
    case ErrorCode::NotFound:
        return errorResponse(error, "order not found", "订单不存在");
    case ErrorCode::AlreadyExists:
        return errorResponse(error, "order already reviewed", "该订单已评价，不能重复修改");
    case ErrorCode::InvalidStateTransition:
        return errorResponse(error, "order is not settled", "仅已完成并结算的订单可以评价");
    default:
        return errorResponse(error, "invalid review", "请选择 1～5 星并填写 1～500 字评价");
    }
}
crow::response stationReviewError(const core::domain::ErrorCode error)
{
    if (error == core::domain::ErrorCode::NotFound)
        return errorResponse(error, "station not found", "未找到相关数据");
    return reviewError(error);
}
QJsonObject stationReviewsJson(const std::vector<core::application::StationReviewView>& reviews)
{
    QJsonArray items;
    for (const auto& review : reviews)
        items.append(QJsonObject{{"author", QString::fromStdString(review.author)},
                                 {"rating", review.rating},
                                 {"content", QString::fromStdString(review.content)},
                                 {"createdAt", QJsonValue(static_cast<qint64>(review.createdAt))}});
    return {{QStringLiteral("items"), items}};
}
} // namespace

OrderReviewRoutes::OrderReviewRoutes(ApiRoutes& routes,
                                     core::application::OrderReviewService& reviews,
                                     core::application::SessionManager& sessions,
                                     core::application::BoundedExecutor& executor,
                                     core::application::IdempotencyService& idempotency)
{
    routes.route("/user/orders/<string>/review")
        .methods(crow::HTTPMethod::GET)(
            [&reviews, &sessions, &executor](const crow::request& request, crow::response& response,
                                             std::string orderNo)
            {
                crow::response failure;
                const auto userId = requireUserId(request, sessions, failure);
                if (!userId)
                {
                    response = std::move(failure);
                    response.end();
                    return;
                }
                dispatchBlocking(
                    request, response, executor,
                    [&reviews, userId = *userId, orderNo]
                    {
                        const auto result = reviews.get(userId, orderNo);
                        if (!result.ok())
                            return reviewError(result.error);
                        return successResponse(QJsonObject{
                            {"review", *result.value ? QJsonValue(reviewJson(**result.value))
                                                     : QJsonValue(QJsonValue::Null)}});
                    });
            });
    routes.route("/user/orders/<string>/review")
        .methods(crow::HTTPMethod::POST)(
            [&reviews, &sessions, &executor, &idempotency](
                const crow::request& request, crow::response& response, std::string orderNo)
            {
                crow::response failure;
                const auto userId = requireUserId(request, sessions, failure);
                if (!userId)
                {
                    response = std::move(failure);
                    response.end();
                    return;
                }
                const auto parsed =
                    parseJsonObject(request, {"rating", "content"}, {"rating", "content"});
                if (!parsed.object || !validIntegerField(*parsed.object, "rating", 1, 5) ||
                    !parsed.object->value("content").isString())
                {
                    response = reviewError(core::domain::ErrorCode::ValidationFailed);
                    response.end();
                    return;
                }
                const auto content = parsed.object->value("content").toString().trimmed();
                if (content.isEmpty() || content.toUcs4().size() > 500 ||
                    content.contains(QChar(0)))
                {
                    response = reviewError(core::domain::ErrorCode::ValidationFailed);
                    response.end();
                    return;
                }
                const int rating = parsed.object->value("rating").toInt();
                dispatchIdempotentBlocking(
                    request, response, executor, idempotency,
                    "u" + std::to_string(*userId) + ":review:" + orderNo, true,
                    [&reviews, userId = *userId, orderNo, rating,
                     content = content.toStdString()](core::application::IdempotencyLease&)
                    {
                        const auto result = reviews.submit(userId, orderNo, rating, content,
                                                           std::chrono::system_clock::now());
                        return result.ok() ? successResponse(reviewJson(*result.value))
                                           : reviewError(result.error);
                    });
            });

    // UC-U-12 场站评论墙：只读，作者展示名由服务层脱敏。
    routes.route("/user/stations/<int>/reviews")
        .methods(crow::HTTPMethod::GET)(
            [&reviews, &sessions, &executor](const crow::request& request, crow::response& response,
                                             std::int64_t stationId)
            {
                crow::response failure;
                const auto userId = requireUserId(request, sessions, failure);
                if (!userId)
                {
                    response = std::move(failure);
                    response.end();
                    return;
                }
                dispatchBlocking(request, response, executor,
                                 [&reviews, stationId]
                                 {
                                     const auto result =
                                         reviews.stationReviews(stationId, kStationReviewWallLimit);
                                     return result.ok()
                                                ? successResponse(stationReviewsJson(*result.value))
                                                : stationReviewError(result.error);
                                 });
            });
}
} // namespace ncs::server::controller
