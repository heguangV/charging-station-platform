#include "core/application/order_review_service.h"

#include <algorithm>

namespace ncs::core::application
{
namespace
{
bool validContent(const std::string& text)
{
    if (text.empty() || text.size() > 2000 || text.find('\0') != std::string::npos)
        return false;
    // JSON decoder supplies valid UTF-8; count codepoints, not UTF-8 bytes.
    const auto count =
        std::count_if(text.begin(), text.end(), [](unsigned char c) { return (c & 0xc0) != 0x80; });
    return count >= 1 && count <= 500 && text.find_first_not_of(" \t\n\r") != std::string::npos;
}

// UC-U-12 评论墙作者脱敏：昵称非空显示昵称（含注销后的“已注销用户”占位），否则退回掩码手机号。
std::string maskedPhone(const std::string_view phone)
{
    if (phone.size() != 11)
        return "***";
    return std::string(phone.substr(0, 3)).append("****").append(phone.substr(7));
}

std::string reviewAuthor(const StationReviewRow& row)
{
    return row.nickname.empty() ? maskedPhone(row.phone) : row.nickname;
}
} // namespace

ServiceResult<std::optional<OrderReview>> OrderReviewService::get(const std::int64_t userId,
                                                                  const std::string& orderNo)
{
    const auto order = orders_.order(orderNo);
    if (!order || order->userId != userId)
        return {core::domain::ErrorCode::NotFound, std::nullopt};
    return {core::domain::ErrorCode::Ok, reviews_.orderReview(orderNo)};
}

ServiceResult<OrderReview>
OrderReviewService::submit(const std::int64_t userId, const std::string& orderNo, const int rating,
                           const std::string& content,
                           const std::chrono::system_clock::time_point now)
{
    using core::domain::ErrorCode;
    if (rating < 1 || rating > 5 || !validContent(content))
        return {ErrorCode::ValidationFailed, std::nullopt};
    ServiceResult<OrderReview> result;
    orders_.withTransaction(
        [&]
        {
            const auto order = orders_.order(orderNo);
            if (!order || order->userId != userId)
            {
                result.error = ErrorCode::NotFound;
                return;
            }
            if (order->status != static_cast<int>(FlowStatus::Completed) || !order->settledAt)
            {
                result.error = ErrorCode::InvalidStateTransition;
                return;
            }
            if (const auto previous = reviews_.orderReview(orderNo))
            {
                if (previous->rating == rating && previous->content == content)
                    result.value = *previous;
                else
                    result.error = ErrorCode::AlreadyExists;
                return;
            }
            OrderReview review{
                orderNo, userId, rating, content,
                std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count()};
            reviews_.addOrderReview(review);
            result.value = std::move(review);
        });
    return result;
}

ServiceResult<std::vector<StationReviewView>>
OrderReviewService::stationReviews(const std::int64_t stationId, const std::size_t limit)
{
    using core::domain::ErrorCode;
    if (!orders_.station(stationId))
        return {ErrorCode::NotFound, std::nullopt};
    std::vector<StationReviewView> views;
    for (const auto& row : reviews_.stationReviewRows(stationId, limit))
        views.push_back({reviewAuthor(row), row.rating, row.content, row.createdAt});
    return {ErrorCode::Ok, std::move(views)};
}

} // namespace ncs::core::application
