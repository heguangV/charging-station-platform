// UC-U-12：订单评价；服务负责所有权、结算状态和每单唯一，存储由仓储端口提供。
// 已提交的评价同时进入所属场站的只读评论墙，作者展示名在本服务内脱敏后才允许出站。
#pragma once

#include "core/application/charging_repository.h"
#include "core/application/service_result.h"

#include <cstddef>
#include <vector>

namespace ncs::core::application
{

struct OrderReview
{
    std::string orderNo;
    std::int64_t userId = 0;
    int rating = 0;
    std::string content;
    std::int64_t createdAt = 0;
};

// 评论墙仓储行：评价原文加推导展示作者所需的账号字段；phone 不得离开应用层明文出站。
struct StationReviewRow
{
    std::int64_t userId = 0;
    std::string nickname;
    std::string phone;
    int rating = 0;
    std::string content;
    std::int64_t createdAt = 0;
};

// 评论墙出站视图：作者已脱敏，不含用户与订单标识。
struct StationReviewView
{
    std::string author;
    int rating = 0;
    std::string content;
    std::int64_t createdAt = 0;
};

class OrderReviewRepository
{
  public:
    virtual ~OrderReviewRepository() = default;
    virtual std::optional<OrderReview> orderReview(const std::string& orderNo) = 0;
    // Called inside ChargingRepository::withTransaction on the same store.
    virtual void addOrderReview(const OrderReview& review) = 0;
    // Latest-first settled-order review rows at a station, capped to limit.
    virtual std::vector<StationReviewRow> stationReviewRows(std::int64_t stationId,
                                                            std::size_t limit) = 0;
};

class OrderReviewService final
{
  public:
    OrderReviewService(ChargingRepository& orders, OrderReviewRepository& reviews)
        : orders_(orders), reviews_(reviews)
    {
    }
    ServiceResult<std::optional<OrderReview>> get(std::int64_t userId, const std::string& orderNo);
    // content must be normalized (Unicode whitespace trimmed) at the protocol boundary.
    ServiceResult<OrderReview> submit(std::int64_t userId, const std::string& orderNo, int rating,
                                      const std::string& content,
                                      std::chrono::system_clock::time_point now);
    // UC-U-12 场站评论墙：只读、按时间倒序、限 limit 条；场站不存在返回 NotFound。
    ServiceResult<std::vector<StationReviewView>> stationReviews(std::int64_t stationId,
                                                                 std::size_t limit);

  private:
    ChargingRepository& orders_;
    OrderReviewRepository& reviews_;
};

} // namespace ncs::core::application
