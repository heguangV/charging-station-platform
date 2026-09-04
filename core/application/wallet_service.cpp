#include "core/application/wallet_service.h"

#include <algorithm>

namespace ncs::core::application {

WalletOverview WalletService::overview(const std::int64_t userId) const
{
    const WalletAccount wallet = repository_.wallet(userId);
    WalletOverview overview;
    overview.balanceCent = wallet.balanceCent;
    overview.debtCent = wallet.debtCent;
    overview.availableCent = wallet.balanceCent;
    overview.version = wallet.version;
    overview.updatedAt = wallet.updatedAt;
    return overview;
}

ServiceResult<RechargeResult> WalletService::recharge(
    const std::int64_t userId,
    const std::int64_t amountCent,
    const std::chrono::system_clock::time_point now)
{
    if (amountCent < 1 || amountCent > maximumRechargeCent) {
        return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
    }
    RechargeResult result;
    const auto nowSeconds = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    repository_.withTransaction([&] {
        WalletAccount wallet = repository_.wallet(userId);
        const std::int64_t debtPaid = std::min(wallet.debtCent, amountCent);
        const std::int64_t balanceAdded = amountCent - debtPaid;
        wallet.balanceCent += balanceAdded;
        wallet.debtCent -= debtPaid;
        ++wallet.version;
        wallet.updatedAt = nowSeconds;

        RechargeOrder order;
        order.rechargeNo = numbers_.next("RC", now);
        order.userId = userId;
        order.requestedCent = amountCent;
        order.debtPaidCent = debtPaid;
        order.balanceAddedCent = balanceAdded;
        order.balanceAfterCent = wallet.balanceCent;
        order.debtAfterCent = wallet.debtCent;
        order.completedAt = nowSeconds;

        WalletTransaction transaction;
        transaction.userId = userId;
        transaction.transactionNo = numbers_.next("WT", now);
        transaction.type = WalletTransactionType::Recharge;
        transaction.amountCent = amountCent;
        transaction.balanceAfterCent = wallet.balanceCent;
        transaction.debtAfterCent = wallet.debtCent;
        transaction.relatedNo = order.rechargeNo;
        transaction.createdAt = nowSeconds;

        repository_.saveWallet(wallet);
        repository_.addRechargeOrder(order);
        repository_.addWalletTransaction(transaction);
        mirror_.applyWalletState(userId, wallet.balanceCent, wallet.debtCent);

        result.rechargeNo = order.rechargeNo;
        result.requestedCent = amountCent;
        result.debtPaidCent = debtPaid;
        result.balanceAddedCent = balanceAdded;
        result.balanceAfterCent = wallet.balanceCent;
        result.debtAfterCent = wallet.debtCent;
        result.completedAt = nowSeconds;
    });
    return {core::domain::ErrorCode::Ok, result};
}

ServiceResult<WalletTransactionPage> WalletService::transactions(
    const std::int64_t userId,
    const std::string &typeFilter,
    const std::int64_t fromAt,
    const std::int64_t toAt,
    const int page,
    const int pageSize) const
{
    std::optional<WalletTransactionType> type;
    if (!typeFilter.empty()) {
        type = walletTransactionTypeFromName(typeFilter);
        if (!type) return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
    }
    const std::vector<WalletTransaction> all =
        repository_.walletTransactions(userId, type, fromAt, toAt);
    WalletTransactionPage result;
    result.total = static_cast<int>(all.size());
    result.page = page;
    result.pageSize = pageSize;
    const auto first = static_cast<std::size_t>(page - 1) * pageSize;
    for (std::size_t index = first;
         index < all.size() && result.items.size() < static_cast<std::size_t>(pageSize);
         ++index) {
        const WalletTransaction &transaction = all[index];
        result.items.push_back(WalletTransactionView{
            transaction.transactionNo,
            walletTransactionTypeName(transaction.type),
            transaction.amountCent,
            transaction.balanceAfterCent,
            transaction.debtAfterCent,
            transaction.relatedNo,
            transaction.createdAt,
        });
    }
    return {core::domain::ErrorCode::Ok, result};
}

} // namespace ncs::core::application
