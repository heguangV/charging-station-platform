// 钱包应用服务：余额/欠款概览、模拟充值（先抵扣欠款、余额入账）与流水分页查询。
// 账本唯一事实来源是 ChargingRepository（钱包与流水表）；用户资料侧镜像经 WalletMirror 同步。
// 约束：金额一律整数分；单笔充值上限 1000000 分（即 1 万元）；充值与流水写入在同一仓储事务内完成。

#pragma once

#include "core/application/business_numbers.h"
#include "core/application/charging_repository.h"
#include "core/application/service_result.h"
#include "core/application/user_account_repository.h"
#include "core/domain/error_code.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ncs::core::application
{

struct WalletOverview
{
    std::int64_t balanceCent = 0;
    std::int64_t debtCent = 0;
    std::int64_t availableCent = 0;
    std::int64_t version = 1;
    std::int64_t updatedAt = 0;
};

struct WalletTransactionView
{
    std::string transactionNo;
    std::string type;
    std::int64_t amountCent = 0;
    std::int64_t balanceAfterCent = 0;
    std::int64_t debtAfterCent = 0;
    std::string relatedNo;
    std::int64_t createdAt = 0;
};

struct WalletTransactionPage
{
    std::vector<WalletTransactionView> items;
    int total = 0;
    int page = 1;
    int pageSize = 20;
};

struct RechargeResult
{
    std::string rechargeNo;
    std::int64_t requestedCent = 0;
    std::int64_t debtPaidCent = 0;
    std::int64_t balanceAddedCent = 0;
    std::int64_t balanceAfterCent = 0;
    std::int64_t debtAfterCent = 0;
    std::int64_t completedAt = 0;
};

// Money values live in the charging store; the user account row keeps a
// mirrored snapshot for profile responses (single source per SQLite contract).

class WalletService final
{
  public:
    WalletService(ChargingRepository& repository, WalletMirror& mirror, BusinessNumbers& numbers)
        : repository_(repository), mirror_(mirror), numbers_(numbers)
    {
    }

    // ---- 查询：钱包概览与流水分页 ----
    WalletOverview overview(std::int64_t userId) const;
    // ---- 充值：先还欠款后入余额，全部在 withTransaction 内完成 ----
    // 模拟充值（事务内）：金额须在 1~maximumRechargeCent 分否则
    // ValidationFailed；入账先抵扣欠款再进余额，
    // 钱包、充值单与流水同一事务落库并同步用户资料侧镜像。
    ServiceResult<RechargeResult> recharge(std::int64_t userId, std::int64_t amountCent,
                                           std::chrono::system_clock::time_point now);
    // 流水分页查询：typeFilter 非空时须为合法流水类型名，否则 ValidationFailed；fromAt/toAt 为 UTC
    // 秒过滤区间。
    ServiceResult<WalletTransactionPage> transactions(std::int64_t userId,
                                                      const std::string& typeFilter,
                                                      std::int64_t fromAt, std::int64_t toAt,
                                                      int page, int pageSize) const;

    static constexpr std::int64_t maximumRechargeCent = 1000000;

  private:
    ChargingRepository& repository_;
    WalletMirror& mirror_;
    BusinessNumbers& numbers_;
};

} // namespace ncs::core::application
