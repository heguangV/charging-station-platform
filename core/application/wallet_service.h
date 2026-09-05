#pragma once

#include "core/application/business_numbers.h"
#include "core/application/service_result.h"
#include "core/application/charging_repository.h"
#include "core/application/user_account_repository.h"
#include "core/domain/error_code.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ncs::core::application {

struct WalletOverview {
    std::int64_t balanceCent = 0;
    std::int64_t debtCent = 0;
    std::int64_t availableCent = 0;
    std::int64_t version = 1;
    std::int64_t updatedAt = 0;
};

struct WalletTransactionView {
    std::string transactionNo;
    std::string type;
    std::int64_t amountCent = 0;
    std::int64_t balanceAfterCent = 0;
    std::int64_t debtAfterCent = 0;
    std::string relatedNo;
    std::int64_t createdAt = 0;
};

struct WalletTransactionPage {
    std::vector<WalletTransactionView> items;
    int total = 0;
    int page = 1;
    int pageSize = 20;
};

struct RechargeResult {
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

class WalletService final {
public:
    WalletService(ChargingRepository &repository, WalletMirror &mirror, BusinessNumbers &numbers)
        : repository_(repository),
          mirror_(mirror),
          numbers_(numbers)
    {
    }

    WalletOverview overview(std::int64_t userId) const;
    ServiceResult<RechargeResult> recharge(
        std::int64_t userId,
        std::int64_t amountCent,
        std::chrono::system_clock::time_point now);
    ServiceResult<WalletTransactionPage> transactions(
        std::int64_t userId,
        const std::string &typeFilter,
        std::int64_t fromAt,
        std::int64_t toAt,
        int page,
        int pageSize) const;

    static constexpr std::int64_t maximumRechargeCent = 1000000;

private:
    ChargingRepository &repository_;
    WalletMirror &mirror_;
    BusinessNumbers &numbers_;
};

} // namespace ncs::core::application
