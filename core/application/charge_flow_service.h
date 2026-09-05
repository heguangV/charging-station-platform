#pragma once

#include "core/application/business_numbers.h"
#include "core/application/service_result.h"
#include "core/application/charging_repository.h"
#include "core/application/pricing.h"
#include "core/application/service_result.h"
#include "core/application/user_account_repository.h"
#include "core/application/wallet_service.h"
#include "core/domain/error_code.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ncs::core::application {

struct FlowQuoteView {
    std::string quoteNo;
    std::int64_t chargerId = 0;
    std::string chargerCode;
    int electricityPriceCentPerKwh = 0;
    int baseServicePriceCentPerKwh = 0;
    int queueAdjustmentBp = 0;
    int mlAdjustmentBp = 0;
    int finalServicePriceCentPerKwh = 0;
    int totalPriceCentPerKwh = 0;
    std::int64_t expiresAt = 0;
};

struct FlowView {
    std::string flowNo;
    std::int64_t stationId = 0;
    ChargerType chargerType = ChargerType::DcFast;
    std::optional<std::int64_t> chargerId;
    std::optional<std::string> chargerCode;
    int status = 10;
    std::string statusText;
    std::optional<int> queuePosition;
    std::optional<FlowQuoteView> quote;
    std::optional<std::int64_t> reservedUntil;
    std::optional<std::int64_t> startedAt;
    std::int64_t version = 1;
};

struct ActiveFlowView {
    bool hasActiveFlow = false;
    std::optional<FlowView> flow;
};

struct QuoteConfirmationView {
    std::string flowNo;
    std::string orderNo;
    int status = 30;
    std::int64_t chargerId = 0;
    std::string chargerCode;
    std::int64_t reservedUntil = 0;
    std::int64_t version = 3;
};

struct ChargeStartView {
    std::string flowNo;
    std::string orderNo;
    int status = 40;
    std::int64_t startedAt = 0;
    std::int64_t powerWatt = 0;
    int timeScale = 60;
    std::int64_t version = 4;
};

struct ChargeProgressView {
    std::string flowNo;
    std::string orderNo;
    int status = 40;
    std::string statusText;
    std::int64_t durationSec = 0;
    std::int64_t energyMwh = 0;
    std::int64_t amountCent = 0;
    std::int64_t powerWatt = 0;
    int simulatedSoc = 0;
    std::int64_t calculatedAt = 0;
};

struct SettlementReceipt {
    std::string flowNo;
    std::string orderNo;
    std::string stationName;
    std::string chargerCode;
    std::int64_t startedAt = 0;
    std::int64_t endedAt = 0;
    std::int64_t durationSec = 0;
    std::int64_t energyMwh = 0;
    int electricityPriceCentPerKwh = 0;
    int servicePriceCentPerKwh = 0;
    std::int64_t amountCent = 0;
    std::int64_t paidCent = 0;
    std::int64_t debtAddedCent = 0;
    std::int64_t balanceAfterCent = 0;
    std::int64_t debtAfterCent = 0;
    std::int64_t settledAt = 0;
    int status = 60;
    std::string statusText;
};

struct OrderSummaryView {
    std::string orderNo;
    std::string flowNo;
    std::string stationName;
    std::string chargerCode;
    int status = 60;
    std::string statusText;
    std::optional<std::int64_t> startedAt;
    std::optional<std::int64_t> endedAt;
    std::int64_t energyMwh = 0;
    std::int64_t amountCent = 0;
};

struct OrderPage {
    std::vector<OrderSummaryView> items;
    int total = 0;
    int page = 1;
    int pageSize = 20;
};

// User-side charging state machine (UC-U-06/07/08/09, BR-02/03/04/12) plus the
// queue, expiry and recovery duties that the runtime tick drives (section 5.5).
class ChargeFlowService final {
public:
    using PriceAdjustmentLookup = std::function<std::int64_t(
        std::int64_t stationId, int chargerType, std::int64_t at)>;

    ChargeFlowService(
        ChargingRepository &repository,
        UserAccountRepository &accounts,
        WalletMirror &walletMirror,
        BusinessNumbers &numbers,
        int chargeTimeScale,
        PriceAdjustmentLookup adjustmentLookup = {})
        : repository_(repository),
          accounts_(accounts),
          walletMirror_(walletMirror),
          numbers_(numbers),
          chargeTimeScale_(chargeTimeScale),
          adjustmentLookup_(std::move(adjustmentLookup))
    {
    }

    ServiceResult<FlowView> createFlow(
        std::int64_t userId,
        std::int64_t stationId,
        int chargerType,
        std::optional<std::int64_t> preferredChargerId,
        std::chrono::system_clock::time_point now);
    ActiveFlowView activeFlow(std::int64_t userId, std::chrono::system_clock::time_point now);
    ServiceResult<FlowView> flowView(
        std::int64_t userId,
        const std::string &flowNo,
        std::chrono::system_clock::time_point now);
    ServiceResult<QuoteConfirmationView> confirmQuote(
        std::int64_t userId,
        const std::string &flowNo,
        const std::string &quoteNo,
        std::int64_t flowVersion,
        std::chrono::system_clock::time_point now);
    ServiceResult<FlowView> cancel(
        std::int64_t userId,
        const std::string &flowNo,
        const std::string &reasonCode,
        std::int64_t flowVersion,
        std::chrono::system_clock::time_point now);
    ServiceResult<ChargeStartView> start(
        std::int64_t userId,
        const std::string &flowNo,
        std::int64_t flowVersion,
        std::optional<std::int64_t> targetAmountCent,
        std::optional<std::int64_t> balanceFloorCent,
        std::chrono::system_clock::time_point now);
    ServiceResult<ChargeProgressView> progress(
        std::int64_t userId,
        const std::string &flowNo,
        std::chrono::system_clock::time_point now);
    ServiceResult<SettlementReceipt> settle(
        std::int64_t userId,
        const std::string &flowNo,
        std::int64_t flowVersion,
        const std::string &reasonCode,
        std::chrono::system_clock::time_point now);
    ServiceResult<OrderPage> orders(
        std::int64_t userId,
        std::optional<int> status,
        std::int64_t fromAt,
        std::int64_t toAt,
        const std::string &sort,
        int page,
        int pageSize) const;
    ServiceResult<SettlementReceipt> receipt(std::int64_t userId, const std::string &orderNo) const;

    // Runtime tick entries: expire quotes and reservations, promote the FIFO
    // queues, and repair account mirrors after a restart.
    void runMaintenance(std::chrono::system_clock::time_point now);
    int recoverAtStartup(std::chrono::system_clock::time_point now);

    // Admin operations (BR-11 / UC-A-05); role and reauthentication checks
    // belong to the caller.
    ServiceResult<FlowView> adminForceRelease(
        const std::string &flowNo,
        const std::string &reason,
        int nextChargerStatus,
        std::int64_t flowVersion,
        std::chrono::system_clock::time_point now);
    ServiceResult<SettlementReceipt> adminControlledSettle(
        const std::string &flowNo,
        const std::string &reason,
        int nextChargerStatus,
        std::chrono::system_clock::time_point now);
    ServiceResult<SettlementReceipt> adminControlledSettle(
        const std::string &flowNo, const std::string &reason,
        std::chrono::system_clock::time_point now) {
      return adminControlledSettle(flowNo, reason,
                                   static_cast<int>(ChargerStatus::Idle), now);
    }

    void promoteAvailable(std::int64_t stationId, ChargerType type,
                          std::chrono::system_clock::time_point now);

    static constexpr std::int64_t minimumStartBalanceCent = 500;
    static constexpr int quoteValiditySec = 5 * 60;
    static constexpr int reservationValiditySec = 15 * 60;
    static constexpr std::int64_t simulatedBatteryCapacityMwh = 60000000;
    static constexpr int initialSimulatedSoc = 20;

private:
    static bool validChargerType(int type) { return type == 0 || type == 1; }
    ServiceResult<FlowView> promoteQueueLocked(
        std::int64_t stationId,
        ChargerType type,
        std::chrono::system_clock::time_point now);

    ChargingRepository &repository_;
    UserAccountRepository &accounts_;
    WalletMirror &walletMirror_;
    BusinessNumbers &numbers_;
    int chargeTimeScale_ = 60;
    PriceAdjustmentLookup adjustmentLookup_;
};

} // namespace ncs::core::application
