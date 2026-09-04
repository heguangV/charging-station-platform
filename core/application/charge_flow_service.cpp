#include "core/application/charge_flow_service.h"

#include <algorithm>

namespace ncs::core::application {
namespace {

std::int64_t unixSeconds(std::chrono::system_clock::time_point now) {
  return std::chrono::duration_cast<std::chrono::seconds>(
             now.time_since_epoch())
      .count();
}

std::optional<int> queuePositionOf(ChargingRepository &repository,
                                   const ChargingFlow &flow) {
  const auto queue = repository.queue(flow.stationId, flow.chargerType);
  int position = 1;
  for (const auto &entry : queue) {
    if (entry == flow.flowNo)
      return position;
    ++position;
  }
  return std::nullopt;
}

std::optional<SettlementReceipt> toReceipt(const ChargingOrder &order) {
  if (!order.endedAt || !order.startedAt)
    return std::nullopt;
  SettlementReceipt receipt;
  receipt.flowNo = order.flowNo;
  receipt.orderNo = order.orderNo;
  receipt.stationName = order.stationName;
  receipt.chargerCode = order.chargerCode;
  receipt.startedAt = *order.startedAt;
  receipt.endedAt = *order.endedAt;
  receipt.durationSec = (*order.endedAt - *order.startedAt) * order.timeScale;
  receipt.energyMwh = order.energyMwh;
  receipt.electricityPriceCentPerKwh = order.electricityPriceCentPerKwh;
  receipt.servicePriceCentPerKwh = order.servicePriceCentPerKwh;
  receipt.amountCent = order.amountCent;
  receipt.paidCent = order.paidCent;
  receipt.debtAddedCent = order.debtAddedCent;
  receipt.balanceAfterCent = order.balanceAfterCent;
  receipt.debtAfterCent = order.debtAfterCent;
  receipt.settledAt = *order.settledAt;
  receipt.status = order.status;
  receipt.statusText = orderStatusText(order.status);
  return receipt;
}

FlowQuoteView toQuoteView(const FlowQuote &quote) {
  FlowQuoteView view;
  view.quoteNo = quote.quoteNo;
  view.chargerId = quote.chargerId;
  view.chargerCode = quote.chargerCode;
  view.electricityPriceCentPerKwh = quote.electricityPriceCentPerKwh;
  view.baseServicePriceCentPerKwh = quote.baseServicePriceCentPerKwh;
  view.queueAdjustmentBp = quote.queueAdjustmentBp;
  view.mlAdjustmentBp = quote.mlAdjustmentBp;
  view.finalServicePriceCentPerKwh = quote.finalServicePriceCentPerKwh;
  view.totalPriceCentPerKwh = quote.totalPriceCentPerKwh;
  view.expiresAt = quote.expiresAt;
  return view;
}

FlowView toFlowView(const ChargingFlow &flow,
                    std::optional<int> queuePosition) {
  FlowView view;
  view.flowNo = flow.flowNo;
  view.stationId = flow.stationId;
  view.chargerType = flow.chargerType;
  view.chargerId = flow.chargerId;
  view.chargerCode = flow.chargerCode;
  view.status = flow.status;
  view.statusText = flowStatusText(flow.status);
  view.queuePosition = queuePosition;
  if (flow.quote)
    view.quote = toQuoteView(*flow.quote);
  view.reservedUntil = flow.reservedUntil;
  view.startedAt = flow.startedAt;
  view.version = flow.version;
  return view;
}

bool validFlowVersion(const ChargingFlow &flow,
                      const std::int64_t flowVersion) {
  return flowVersion > 0 && flow.version == flowVersion;
}

bool validReasonText(const std::string &reason) {
  if (reason.size() < 2 || reason.size() > 200)
    return false;
  for (const unsigned char character : reason) {
    if (character < 0x20 || character == 0x7f)
      return false;
  }
  return true;
}

bool validReasonCode(const std::string &reasonCode) {
  return !reasonCode.empty() && reasonCode.size() <= 32 &&
         reasonCode.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmn"
                                      "opqrstuvwxyz0123456789_") ==
             std::string::npos;
}

} // namespace

ServiceResult<FlowView> ChargeFlowService::createFlow(
    const std::int64_t userId, const std::int64_t stationId,
    const int chargerType, const std::optional<std::int64_t> preferredChargerId,
    const std::chrono::system_clock::time_point now) {
  if (!validChargerType(chargerType))
    return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
  const auto type = static_cast<ChargerType>(chargerType);
  const auto nowSeconds = unixSeconds(now);
  core::domain::ErrorCode error = core::domain::ErrorCode::Ok;
  ChargingFlow flow;
  std::optional<int> queuePosition;
  repository_.withTransaction([&] {
    const auto account = accounts_.findById(userId);
    if (!account || account->deleted) {
      error = core::domain::ErrorCode::NotFound;
      return;
    }
    if (account->status != 1) {
      error = core::domain::ErrorCode::UserFrozen;
      return;
    }
    if (repository_.activeFlow(userId)) {
      error = core::domain::ErrorCode::ActiveFlowExists;
      return;
    }
    const WalletAccount wallet = repository_.wallet(userId);
    if (wallet.debtCent > 0) {
      error = core::domain::ErrorCode::DebtOutstanding;
      return;
    }
    if (wallet.balanceCent < minimumStartBalanceCent) {
      error = core::domain::ErrorCode::InsufficientBalance;
      return;
    }
    const auto station = repository_.station(stationId);
    if (!station) {
      error = core::domain::ErrorCode::NotFound;
      return;
    }
    if (!station->enabled) {
      error = core::domain::ErrorCode::ChargerUnavailable;
      return;
    }
    const auto tariff =
        repository_.effectiveTariff(station->adcode, nowSeconds);
    if (!tariff) {
      error = core::domain::ErrorCode::ChargerUnavailable;
      return;
    }

    // Never let a new request overtake compatible flows already waiting in
    // the station/type FIFO when administrative work has created capacity.
    while (promoteQueueLocked(stationId, type, now).value) {
    }

    std::optional<Charger> allocated;
    if (preferredChargerId) {
      auto candidate = repository_.charger(*preferredChargerId);
      if (candidate && candidate->stationId == stationId &&
          candidate->type == type && candidate->status == ChargerStatus::Idle) {
        allocated = candidate;
      } else {
        error = core::domain::ErrorCode::AllocationConflict;
        return;
      }
    } else {
      const auto idle =
          repository_.chargers(stationId, type, ChargerStatus::Idle);
      if (!idle.empty())
        allocated = idle.front();
    }

    flow.flowNo = numbers_.next("FL", now);
    flow.userId = userId;
    flow.stationId = stationId;
    flow.chargerType = type;
    flow.createdAt = nowSeconds;
    bool queued = false;
    if (allocated) {
      allocated->status = ChargerStatus::Occupied;
      repository_.saveCharger(*allocated);
      repository_.addChargerStatusEvent(
          ChargerStatusEvent{allocated->id, allocated->stationId,
                             static_cast<int>(ChargerStatus::Idle),
                             static_cast<int>(ChargerStatus::Occupied),
                             "ALLOCATED", nowSeconds});
      flow.chargerId = allocated->id;
      flow.chargerCode = allocated->code;
      flow.status = static_cast<int>(FlowStatus::PendingQuote);
      const auto waiting = repository_.queue(stationId, type).size();
      const std::int64_t adjustmentBp =
          adjustmentLookup_
              ? adjustmentLookup_(stationId, chargerType, nowSeconds)
              : 0;
      const PriceBreakdown breakdown = computePrice(
          *tariff, static_cast<int>(waiting), static_cast<int>(adjustmentBp));
      FlowQuote quote;
      quote.quoteNo = numbers_.next("QT", now);
      quote.chargerId = allocated->id;
      quote.chargerCode = allocated->code;
      quote.electricityPriceCentPerKwh = breakdown.electricityPriceCentPerKwh;
      quote.baseServicePriceCentPerKwh = breakdown.baseServicePriceCentPerKwh;
      quote.queueAdjustmentBp = breakdown.queueAdjustmentBp;
      quote.mlAdjustmentBp = breakdown.mlAdjustmentBp;
      quote.finalServicePriceCentPerKwh = breakdown.finalServicePriceCentPerKwh;
      quote.totalPriceCentPerKwh = breakdown.totalPriceCentPerKwh;
      quote.expiresAt = nowSeconds + quoteValiditySec;
      flow.quote = quote;
    } else {
      flow.status = static_cast<int>(FlowStatus::Queued);
      queued = true;
    }
    repository_.addFlow(flow);
    if (queued) {
      repository_.enqueue(stationId, type, flow.flowNo);
      queuePosition =
          static_cast<int>(repository_.queue(stationId, type).size());
    }
    repository_.addFlowEvent(
        FlowEvent{flow.flowNo, 0, flow.status, "CREATED", nowSeconds});
    walletMirror_.setActiveFlowFlag(userId, true);
  });
  if (error != core::domain::ErrorCode::Ok)
    return {error, std::nullopt};
  return {core::domain::ErrorCode::Ok, toFlowView(flow, queuePosition)};
}

ActiveFlowView
ChargeFlowService::activeFlow(const std::int64_t userId,
                              const std::chrono::system_clock::time_point now) {
  (void)now;
  const auto flow = repository_.activeFlow(userId);
  ActiveFlowView view;
  if (!flow)
    return view;
  view.hasActiveFlow = true;
  view.flow =
      toFlowView(*flow, flow->status == static_cast<int>(FlowStatus::Queued)
                            ? queuePositionOf(repository_, *flow)
                            : std::nullopt);
  return view;
}

ServiceResult<FlowView>
ChargeFlowService::flowView(const std::int64_t userId,
                            const std::string &flowNo,
                            const std::chrono::system_clock::time_point now) {
  (void)now;
  const auto flow = repository_.flow(flowNo);
  if (!flow || flow->userId != userId)
    return {core::domain::ErrorCode::NotFound, std::nullopt};
  return {core::domain::ErrorCode::Ok,
          toFlowView(*flow, flow->status == static_cast<int>(FlowStatus::Queued)
                                ? queuePositionOf(repository_, *flow)
                                : std::nullopt)};
}

ServiceResult<QuoteConfirmationView> ChargeFlowService::confirmQuote(
    const std::int64_t userId, const std::string &flowNo,
    const std::string &quoteNo, const std::int64_t flowVersion,
    const std::chrono::system_clock::time_point now) {
  const auto nowSeconds = unixSeconds(now);
  core::domain::ErrorCode error = core::domain::ErrorCode::Ok;
  QuoteConfirmationView view;
  repository_.withTransaction([&] {
    const auto account = accounts_.findById(userId);
    if (!account || account->deleted) {
      error = core::domain::ErrorCode::NotFound;
      return;
    }
    if (account->status != 1) {
      error = core::domain::ErrorCode::UserFrozen;
      return;
    }
    const WalletAccount wallet = repository_.wallet(userId);
    if (wallet.debtCent > 0) {
      error = core::domain::ErrorCode::DebtOutstanding;
      return;
    }
    if (wallet.balanceCent < minimumStartBalanceCent) {
      error = core::domain::ErrorCode::InsufficientBalance;
      return;
    }
    const auto flow = repository_.flow(flowNo);
    if (!flow || flow->userId != userId) {
      error = core::domain::ErrorCode::NotFound;
      return;
    }
    if (flow->status != static_cast<int>(FlowStatus::PendingQuote) ||
        !flow->quote || flow->quote->quoteNo != quoteNo) {
      error = core::domain::ErrorCode::InvalidStateTransition;
      return;
    }
    if (!validFlowVersion(*flow, flowVersion)) {
      error = core::domain::ErrorCode::VersionConflict;
      return;
    }
    if (flow->quote->expiresAt <= nowSeconds) {
      error = core::domain::ErrorCode::QuoteExpired;
      return;
    }
    const auto charger = repository_.charger(flow->quote->chargerId);
    if (!charger || charger->status != ChargerStatus::Occupied) {
      error = core::domain::ErrorCode::ChargerUnavailable;
      return;
    }
    const auto station = repository_.station(flow->stationId);
    const auto tariff =
        station ? repository_.effectiveTariff(station->adcode, nowSeconds)
                : std::nullopt;
    if (!station || !tariff) {
      error = core::domain::ErrorCode::ChargerUnavailable;
      return;
    }

    ChargingOrder order;
    order.orderNo = numbers_.next("OR", now);
    order.flowNo = flow->flowNo;
    order.userId = userId;
    order.stationId = flow->stationId;
    order.stationName = station->name;
    order.chargerId = flow->quote->chargerId;
    order.chargerCode = flow->quote->chargerCode;
    order.chargerType = flow->chargerType;
    order.electricityPriceCentPerKwh = flow->quote->electricityPriceCentPerKwh;
    order.servicePriceCentPerKwh = flow->quote->finalServicePriceCentPerKwh;
    order.powerWatt = charger->powerWatt;
    order.createdAt = nowSeconds;
    order.status = static_cast<int>(FlowStatus::Reserved);

    ChargingFlow updated = *flow;
    updated.status = static_cast<int>(FlowStatus::Reserved);
    updated.reservedUntil = nowSeconds + reservationValiditySec;
    ++updated.version;
    updated.quote.reset();

    repository_.addOrder(order);
    repository_.saveFlow(updated);
    repository_.addFlowEvent(FlowEvent{flowNo, flow->status, updated.status,
                                       "QUOTE_CONFIRMED", nowSeconds});

    view.flowNo = updated.flowNo;
    view.orderNo = order.orderNo;
    view.status = updated.status;
    view.chargerId = order.chargerId;
    view.chargerCode = order.chargerCode;
    view.reservedUntil = *updated.reservedUntil;
    view.version = updated.version;
  });
  if (error != core::domain::ErrorCode::Ok)
    return {error, std::nullopt};
  return {core::domain::ErrorCode::Ok, view};
}

ServiceResult<FlowView>
ChargeFlowService::cancel(const std::int64_t userId, const std::string &flowNo,
                          const std::string &reasonCode,
                          const std::int64_t flowVersion,
                          const std::chrono::system_clock::time_point now) {
  if (!validReasonCode(reasonCode))
    return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
  const auto nowSeconds = unixSeconds(now);
  core::domain::ErrorCode error = core::domain::ErrorCode::Ok;
  FlowView view;
  repository_.withTransaction([&] {
    const auto flow = repository_.flow(flowNo);
    if (!flow || flow->userId != userId) {
      error = core::domain::ErrorCode::NotFound;
      return;
    }
    const bool cancellable =
        flow->status == static_cast<int>(FlowStatus::Queued) ||
        flow->status == static_cast<int>(FlowStatus::PendingQuote) ||
        flow->status == static_cast<int>(FlowStatus::Reserved);
    if (!cancellable) {
      error = core::domain::ErrorCode::InvalidStateTransition;
      return;
    }
    if (!validFlowVersion(*flow, flowVersion)) {
      error = core::domain::ErrorCode::VersionConflict;
      return;
    }
    ChargingFlow updated = *flow;
    updated.status = static_cast<int>(FlowStatus::Cancelled);
    updated.quote.reset();
    updated.reservedUntil.reset();
    ++updated.version;
    if (flow->chargerId) {
      if (auto charger = repository_.charger(*flow->chargerId)) {
        const int fromStatus = static_cast<int>(charger->status);
        charger->status = ChargerStatus::Idle;
        repository_.saveCharger(*charger);
        repository_.addChargerStatusEvent(
            ChargerStatusEvent{charger->id, charger->stationId, fromStatus,
                               static_cast<int>(ChargerStatus::Idle),
                               reasonCode, nowSeconds});
      }
      repository_.dequeue(flow->stationId, flow->chargerType, flowNo);
      updated.chargerId.reset();
      updated.chargerCode.reset();
      repository_.saveFlow(updated);
      repository_.addFlowEvent(FlowEvent{flowNo, flow->status, updated.status,
                                         reasonCode, nowSeconds});
      promoteQueueLocked(flow->stationId, flow->chargerType, now);
    } else {
      repository_.dequeue(flow->stationId, flow->chargerType, flowNo);
      repository_.saveFlow(updated);
      repository_.addFlowEvent(FlowEvent{flowNo, flow->status, updated.status,
                                         reasonCode, nowSeconds});
    }
    if (const auto order = repository_.orderByFlow(flowNo)) {
      if (isActiveFlowStatus(order->status)) {
        ChargingOrder cancelledOrder = *order;
        cancelledOrder.status = static_cast<int>(FlowStatus::Cancelled);
        repository_.saveOrder(cancelledOrder);
      }
    }
    walletMirror_.setActiveFlowFlag(userId, false);
    view = toFlowView(updated, std::nullopt);
  });
  if (error != core::domain::ErrorCode::Ok)
    return {error, std::nullopt};
  return {core::domain::ErrorCode::Ok, view};
}

ServiceResult<ChargeStartView>
ChargeFlowService::start(const std::int64_t userId, const std::string &flowNo,
                         const std::int64_t flowVersion,
                         const std::optional<std::int64_t> targetAmountCent,
                         const std::optional<std::int64_t> balanceFloorCent,
                         const std::chrono::system_clock::time_point now) {
  if (targetAmountCent && *targetAmountCent < 1) {
    return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
  }
  if (balanceFloorCent && *balanceFloorCent < 0) {
    return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
  }
  const auto nowSeconds = unixSeconds(now);
  core::domain::ErrorCode error = core::domain::ErrorCode::Ok;
  ChargeStartView view;
  repository_.withTransaction([&] {
    const auto account = accounts_.findById(userId);
    if (!account || account->deleted) {
      error = core::domain::ErrorCode::NotFound;
      return;
    }
    if (account->status != 1) {
      error = core::domain::ErrorCode::UserFrozen;
      return;
    }
    const auto flow = repository_.flow(flowNo);
    if (!flow || flow->userId != userId) {
      error = core::domain::ErrorCode::NotFound;
      return;
    }
    if (flow->status != static_cast<int>(FlowStatus::Reserved)) {
      error = core::domain::ErrorCode::InvalidStateTransition;
      return;
    }
    if (!validFlowVersion(*flow, flowVersion)) {
      error = core::domain::ErrorCode::VersionConflict;
      return;
    }
    if (!flow->reservedUntil || flow->reservedUntil <= nowSeconds) {
      error = core::domain::ErrorCode::ReservationExpired;
      return;
    }
    const auto order = repository_.orderByFlow(flowNo);
    const auto charger =
        flow->chargerId ? repository_.charger(*flow->chargerId) : std::nullopt;
    if (!order || !charger || charger->status != ChargerStatus::Occupied) {
      error = core::domain::ErrorCode::ChargerUnavailable;
      return;
    }
    const WalletAccount wallet = repository_.wallet(userId);
    // BR-04: the city-wide minimum applies no matter what the client asks for;
    // a client floor can only make the check stricter.
    const std::int64_t floor = std::max(
        minimumStartBalanceCent, balanceFloorCent.value_or(minimumStartBalanceCent));
    if (wallet.debtCent > 0) {
      error = core::domain::ErrorCode::DebtOutstanding;
      return;
    }
    if (wallet.balanceCent < floor) {
      error = core::domain::ErrorCode::InsufficientBalance;
      return;
    }

    ChargingFlow updated = *flow;
    updated.status = static_cast<int>(FlowStatus::Charging);
    updated.startedAt = nowSeconds;
    updated.reservedUntil.reset();
    ++updated.version;

    ChargingOrder startedOrder = *order;
    startedOrder.status = static_cast<int>(FlowStatus::Charging);
    startedOrder.startedAt = nowSeconds;
    startedOrder.timeScale = chargeTimeScale_;
    startedOrder.targetAmountCent = targetAmountCent;

    repository_.saveFlow(updated);
    repository_.saveOrder(startedOrder);
    repository_.addFlowEvent(
        FlowEvent{flowNo, flow->status, updated.status, "STARTED", nowSeconds});

    view.flowNo = updated.flowNo;
    view.orderNo = order->orderNo;
    view.status = updated.status;
    view.startedAt = nowSeconds;
    view.powerWatt = charger->powerWatt;
    view.timeScale = chargeTimeScale_;
    view.version = updated.version;
  });
  if (error != core::domain::ErrorCode::Ok)
    return {error, std::nullopt};
  return {core::domain::ErrorCode::Ok, view};
}

ServiceResult<ChargeProgressView>
ChargeFlowService::progress(const std::int64_t userId,
                            const std::string &flowNo,
                            const std::chrono::system_clock::time_point now) {
  const auto flow = repository_.flow(flowNo);
  if (!flow || flow->userId != userId)
    return {core::domain::ErrorCode::NotFound, std::nullopt};
  if (flow->status != static_cast<int>(FlowStatus::Charging) ||
      !flow->startedAt) {
    return {core::domain::ErrorCode::InvalidStateTransition, std::nullopt};
  }
  const auto order = repository_.orderByFlow(flowNo);
  if (!order)
    return {core::domain::ErrorCode::NotFound, std::nullopt};
  const auto nowSeconds = unixSeconds(now);
  const std::int64_t simulatedSeconds = std::max<std::int64_t>(
      0, (nowSeconds - *flow->startedAt) * order->timeScale);
  const std::int64_t energyMwh =
      energyMwhForDuration(order->powerWatt, simulatedSeconds);
  const int totalPrice =
      order->electricityPriceCentPerKwh + order->servicePriceCentPerKwh;
  ChargeProgressView view;
  view.flowNo = flowNo;
  view.orderNo = order->orderNo;
  view.status = flow->status;
  view.statusText = flowStatusText(flow->status);
  view.durationSec = simulatedSeconds;
  view.energyMwh = energyMwh;
  view.amountCent = amountCentForEnergy(energyMwh, totalPrice);
  view.powerWatt = order->powerWatt;
  view.simulatedSoc = std::min(
      100, initialSimulatedSoc +
               static_cast<int>(energyMwh * 80 / simulatedBatteryCapacityMwh));
  view.calculatedAt = nowSeconds;
  return {core::domain::ErrorCode::Ok, view};
}

ServiceResult<SettlementReceipt>
ChargeFlowService::settle(const std::int64_t userId, const std::string &flowNo,
                          const std::int64_t flowVersion,
                          const std::string &reasonCode,
                          const std::chrono::system_clock::time_point now) {
  if (!validReasonCode(reasonCode))
    return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
  const auto nowSeconds = unixSeconds(now);
  core::domain::ErrorCode error = core::domain::ErrorCode::Ok;
  std::optional<SettlementReceipt> receipt;
  try {
    repository_.withTransaction([&] {
      const auto flow = repository_.flow(flowNo);
      if (!flow || flow->userId != userId) {
        error = core::domain::ErrorCode::NotFound;
        return;
      }
      const bool settleable =
          flow->status == static_cast<int>(FlowStatus::Charging) ||
          flow->status == static_cast<int>(FlowStatus::SettlementFailed);
      if (!settleable) {
        error = core::domain::ErrorCode::InvalidStateTransition;
        return;
      }
      if (!validFlowVersion(*flow, flowVersion)) {
        error = core::domain::ErrorCode::VersionConflict;
        return;
      }
      const auto order = repository_.orderByFlow(flowNo);
      if (!order || !order->startedAt) {
        error = core::domain::ErrorCode::InvalidStateTransition;
        return;
      }

      // The settling state is transient inside this transaction; the client
      // observes a single version step from charging to the final status.
      ChargingFlow settling = *flow;
      settling.status = static_cast<int>(FlowStatus::Settling);
      repository_.saveFlow(settling);

      const std::int64_t simulatedSeconds = std::max<std::int64_t>(
          0, (nowSeconds - *order->startedAt) * order->timeScale);
      const std::int64_t energyMwh =
          energyMwhForDuration(order->powerWatt, simulatedSeconds);
      const int totalPrice =
          order->electricityPriceCentPerKwh + order->servicePriceCentPerKwh;
      const std::int64_t amountCent =
          amountCentForEnergy(energyMwh, totalPrice);

      WalletAccount wallet = repository_.wallet(userId);
      const std::int64_t paidCent = std::min(wallet.balanceCent, amountCent);
      const std::int64_t debtAddedCent = amountCent - paidCent;
      wallet.balanceCent -= paidCent;
      wallet.debtCent += debtAddedCent;
      ++wallet.version;
      wallet.updatedAt = nowSeconds;

      WalletTransaction transaction;
      transaction.userId = userId;
      transaction.transactionNo = numbers_.next("WT", now);
      transaction.type = WalletTransactionType::Charge;
      transaction.amountCent = -paidCent;
      transaction.balanceAfterCent = wallet.balanceCent;
      transaction.debtAfterCent = wallet.debtCent;
      transaction.relatedNo = order->orderNo;
      transaction.createdAt = nowSeconds;

      ChargingOrder settledOrder = *order;
      settledOrder.status = static_cast<int>(FlowStatus::Completed);
      settledOrder.endedAt = nowSeconds;
      settledOrder.energyMwh = energyMwh;
      settledOrder.amountCent = amountCent;
      settledOrder.paidCent = paidCent;
      settledOrder.debtAddedCent = debtAddedCent;
      settledOrder.balanceAfterCent = wallet.balanceCent;
      settledOrder.debtAfterCent = wallet.debtCent;
      settledOrder.settledAt = nowSeconds;

      ChargingFlow completed = settling;
      completed.status = static_cast<int>(FlowStatus::Completed);
      ++completed.version;
      if (completed.chargerId) {
        if (auto charger = repository_.charger(*completed.chargerId)) {
          const int fromStatus = static_cast<int>(charger->status);
          charger->status = ChargerStatus::Idle;
          charger->totalCount += 1;
          charger->totalMinutes += simulatedSeconds / 60;
          repository_.saveCharger(*charger);
          repository_.addChargerStatusEvent(
              ChargerStatusEvent{charger->id, charger->stationId, fromStatus,
                                 static_cast<int>(ChargerStatus::Idle),
                                 reasonCode, nowSeconds});
        }
      }

      repository_.saveWallet(wallet);
      repository_.addWalletTransaction(transaction);
      repository_.saveOrder(settledOrder);
      repository_.saveFlow(completed);
      repository_.addFlowEvent(FlowEvent{
          flowNo, settling.status, completed.status, reasonCode, nowSeconds});
      walletMirror_.applyWalletState(userId, wallet.balanceCent,
                                     wallet.debtCent);
      walletMirror_.setActiveFlowFlag(userId, false);

      if (completed.chargerId) {
        promoteQueueLocked(flow->stationId, flow->chargerType, now);
      }
      receipt = toReceipt(settledOrder);
    });
  } catch (...) {
    // The settlement transaction has rolled back. Persist the recoverable
    // terminal of this attempt in a separate transaction so a retry can
    // safely resume from status 80.
    try {
      repository_.withTransaction([&] {
        const auto current = repository_.flow(flowNo);
        if (!current || current->userId != userId ||
            (current->status != static_cast<int>(FlowStatus::Charging) &&
             current->status != static_cast<int>(FlowStatus::Settling) &&
             current->status !=
                 static_cast<int>(FlowStatus::SettlementFailed))) {
          return;
        }
        ChargingFlow failed = *current;
        failed.status = static_cast<int>(FlowStatus::SettlementFailed);
        // The version is deliberately unchanged: the documented same-key
        // retry carries the pre-failure version and must pass
        // validFlowVersion directly against the settleable status 80.
        repository_.saveFlow(failed);
        if (const auto order = repository_.orderByFlow(flowNo)) {
          ChargingOrder failedOrder = *order;
          failedOrder.status = static_cast<int>(FlowStatus::SettlementFailed);
          repository_.saveOrder(failedOrder);
        }
        repository_.addFlowEvent(FlowEvent{flowNo, current->status,
                                           failed.status, "SETTLEMENT_FAILED",
                                           nowSeconds});
        walletMirror_.setActiveFlowFlag(userId, true);
      });
    } catch (...) {
      // The public response stays generic; the next startup recovery
      // pass will detect any still-active charging/settling record.
    }
    return {core::domain::ErrorCode::TransactionFailed, std::nullopt};
  }
  if (error != core::domain::ErrorCode::Ok)
    return {error, std::nullopt};
  return {core::domain::ErrorCode::Ok, *receipt};
}

ServiceResult<OrderPage> ChargeFlowService::orders(
    const std::int64_t userId, const std::optional<int> status,
    const std::int64_t fromAt, const std::int64_t toAt, const std::string &sort,
    const int page, const int pageSize) const {
  if (!sort.empty() && sort != "createdAt" && sort != "-createdAt") {
    return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
  }
  if (status && (status < 60 || status > 90)) {
    return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
  }
  auto all = repository_.orders(userId, status, fromAt, toAt);
  if (sort != "createdAt") {
    // Default and -createdAt are descending by creation time.
  } else {
    std::reverse(all.begin(), all.end());
  }
  OrderPage result;
  result.total = static_cast<int>(all.size());
  result.page = page;
  result.pageSize = pageSize;
  const auto first = static_cast<std::size_t>(page - 1) * pageSize;
  for (std::size_t index = first;
       index < all.size() &&
       result.items.size() < static_cast<std::size_t>(pageSize);
       ++index) {
    const ChargingOrder &order = all[index];
    OrderSummaryView view;
    view.orderNo = order.orderNo;
    view.flowNo = order.flowNo;
    view.stationName = order.stationName;
    view.chargerCode = order.chargerCode;
    view.status = order.status;
    view.statusText = orderStatusText(order.status);
    view.startedAt = order.startedAt;
    view.endedAt = order.endedAt;
    view.energyMwh = order.energyMwh;
    view.amountCent = order.amountCent;
    result.items.push_back(std::move(view));
  }
  return {core::domain::ErrorCode::Ok, result};
}

ServiceResult<SettlementReceipt>
ChargeFlowService::receipt(const std::int64_t userId,
                           const std::string &orderNo) const {
  const auto order = repository_.order(orderNo);
  if (!order || order->userId != userId) {
    return {core::domain::ErrorCode::NotFound, std::nullopt};
  }
  if (const auto settled = toReceipt(*order)) {
    return {core::domain::ErrorCode::Ok, *settled};
  }
  return {core::domain::ErrorCode::NotFound, std::nullopt};
}

void ChargeFlowService::runMaintenance(
    const std::chrono::system_clock::time_point now) {
  const auto nowSeconds = unixSeconds(now);
  repository_.withTransaction([&] {
    std::vector<ChargingFlow> timedOut;
    for (const auto &flow : repository_.flowsWithStatus(
             static_cast<int>(FlowStatus::PendingQuote))) {
      if (flow.quote && flow.quote->expiresAt <= nowSeconds)
        timedOut.push_back(flow);
    }
    for (const auto &flow :
         repository_.flowsWithStatus(static_cast<int>(FlowStatus::Reserved))) {
      if (flow.reservedUntil && flow.reservedUntil <= nowSeconds)
        timedOut.push_back(flow);
    }
    for (const auto &flow : timedOut) {
      ChargingFlow expired = flow;
      expired.status = static_cast<int>(FlowStatus::Expired);
      expired.quote.reset();
      expired.reservedUntil.reset();
      ++expired.version;
      if (flow.chargerId) {
        if (auto charger = repository_.charger(*flow.chargerId)) {
          const int fromStatus = static_cast<int>(charger->status);
          charger->status = ChargerStatus::Idle;
          repository_.saveCharger(*charger);
          repository_.addChargerStatusEvent(
              ChargerStatusEvent{charger->id, charger->stationId, fromStatus,
                                 static_cast<int>(ChargerStatus::Idle),
                                 flow.status
                                         == static_cast<int>(
                                                FlowStatus::PendingQuote)
                                     ? "QUOTE_EXPIRED"
                                     : "RESERVATION_EXPIRED",
                                 nowSeconds});
        }
        expired.chargerId.reset();
        expired.chargerCode.reset();
      }
      repository_.saveFlow(expired);
      repository_.addFlowEvent(
          FlowEvent{flow.flowNo, flow.status, expired.status,
                    flow.status == static_cast<int>(FlowStatus::PendingQuote)
                        ? "QUOTE_EXPIRED"
                        : "RESERVATION_EXPIRED",
                    nowSeconds});
      if (const auto order = repository_.orderByFlow(flow.flowNo)) {
        ChargingOrder expiredOrder = *order;
        expiredOrder.status = static_cast<int>(FlowStatus::Expired);
        repository_.saveOrder(expiredOrder);
      }
      walletMirror_.setActiveFlowFlag(flow.userId, false);
      promoteQueueLocked(flow.stationId, flow.chargerType, now);
    }
  });
}

int ChargeFlowService::recoverAtStartup(
    const std::chrono::system_clock::time_point now) {
  int recovered = 0;
  repository_.withTransaction([&] {
    for (const int status : {10, 20, 30, 40, 50, 80}) {
      for (const auto &flow : repository_.flowsWithStatus(status)) {
        ++recovered;
        walletMirror_.setActiveFlowFlag(flow.userId, true);
      }
    }
    // Charging flows resume billing from their persisted startedAt when
    // the next progress or settlement request arrives (NFR-R-01).
  });
  return recovered;
}

ServiceResult<FlowView> ChargeFlowService::promoteQueueLocked(
    const std::int64_t stationId, const ChargerType type,
    const std::chrono::system_clock::time_point now) {
  const auto nowSeconds = unixSeconds(now);
  auto queue = repository_.queue(stationId, type);
  std::optional<Charger> allocated;
  std::string promotedFlowNo;
  while (!queue.empty() && !allocated) {
    const std::string candidateNo = queue.front();
    const auto candidateFlow = repository_.flow(candidateNo);
    if (!candidateFlow ||
        candidateFlow->status != static_cast<int>(FlowStatus::Queued)) {
      queue.pop_front();
      repository_.dequeue(stationId, type, candidateNo);
      continue;
    }
    const auto idle =
        repository_.chargers(stationId, type, ChargerStatus::Idle);
    if (idle.empty())
      break;
    allocated = idle.front();
    promotedFlowNo = candidateNo;
  }
  if (!allocated || promotedFlowNo.empty())
    return {core::domain::ErrorCode::Ok, std::nullopt};
  const auto station = repository_.station(stationId);
  const auto tariff =
      station ? repository_.effectiveTariff(station->adcode, nowSeconds)
              : std::nullopt;
  if (!tariff)
    return {core::domain::ErrorCode::Ok, std::nullopt};
  const auto flow = repository_.flow(promotedFlowNo);
  if (!flow)
    return {core::domain::ErrorCode::Ok, std::nullopt};

  allocated->status = ChargerStatus::Occupied;
  repository_.saveCharger(*allocated);
  repository_.addChargerStatusEvent(
      ChargerStatusEvent{allocated->id, allocated->stationId,
                         static_cast<int>(ChargerStatus::Idle),
                         static_cast<int>(ChargerStatus::Occupied),
                         "PROMOTED", nowSeconds});
  repository_.dequeue(stationId, type, promotedFlowNo);

  const PriceBreakdown breakdown = computePrice(
      *tariff, static_cast<int>(repository_.queue(stationId, type).size()),
      adjustmentLookup_
          ? static_cast<int>(adjustmentLookup_(
                stationId, static_cast<int>(type), nowSeconds))
          : 0);
  ChargingFlow promoted = *flow;
  promoted.status = static_cast<int>(FlowStatus::PendingQuote);
  promoted.chargerId = allocated->id;
  promoted.chargerCode = allocated->code;
  FlowQuote quote;
  quote.quoteNo = numbers_.next("QT", now);
  quote.chargerId = allocated->id;
  quote.chargerCode = allocated->code;
  quote.electricityPriceCentPerKwh = breakdown.electricityPriceCentPerKwh;
  quote.baseServicePriceCentPerKwh = breakdown.baseServicePriceCentPerKwh;
  quote.queueAdjustmentBp = breakdown.queueAdjustmentBp;
  quote.mlAdjustmentBp = breakdown.mlAdjustmentBp;
  quote.finalServicePriceCentPerKwh = breakdown.finalServicePriceCentPerKwh;
  quote.totalPriceCentPerKwh = breakdown.totalPriceCentPerKwh;
  quote.expiresAt = nowSeconds + quoteValiditySec;
  promoted.quote = quote;
  ++promoted.version;
  repository_.saveFlow(promoted);
  repository_.addFlowEvent(FlowEvent{promotedFlowNo, flow->status,
                                     promoted.status, "PROMOTED", nowSeconds});
  return {core::domain::ErrorCode::Ok, toFlowView(promoted, std::nullopt)};
}


ServiceResult<FlowView> ChargeFlowService::adminForceRelease(
    const std::string &flowNo, const std::string &reason,
    const int nextChargerStatus, const std::int64_t flowVersion,
    const std::chrono::system_clock::time_point now) {
  if (!validReasonText(reason) ||
      (nextChargerStatus != 0 && nextChargerStatus != 2 &&
       nextChargerStatus != 3 && nextChargerStatus != 4)) {
    return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
  }
  const auto nowSeconds = unixSeconds(now);
  core::domain::ErrorCode error = core::domain::ErrorCode::Ok;
  std::optional<FlowView> view;
  repository_.withTransaction([&] {
    const auto flow = repository_.flow(flowNo);
    if (!flow) {
      error = core::domain::ErrorCode::NotFound;
      return;
    }
    const bool releasable =
        flow->status == static_cast<int>(FlowStatus::PendingQuote) ||
        flow->status == static_cast<int>(FlowStatus::Reserved);
    if (!releasable) {
      error = core::domain::ErrorCode::InvalidStateTransition;
      return;
    }
    if (!validFlowVersion(*flow, flowVersion)) {
      error = core::domain::ErrorCode::VersionConflict;
      return;
    }
    ChargingFlow updated = *flow;
    updated.status = static_cast<int>(FlowStatus::Cancelled);
    updated.quote.reset();
    updated.reservedUntil.reset();
    ++updated.version;
    if (flow->chargerId) {
      if (auto charger = repository_.charger(*flow->chargerId)) {
        const int fromStatus = static_cast<int>(charger->status);
        charger->status = static_cast<ChargerStatus>(nextChargerStatus);
        ++charger->version;
        repository_.saveCharger(*charger);
        repository_.addChargerStatusEvent(
            ChargerStatusEvent{charger->id, charger->stationId, fromStatus,
                               nextChargerStatus, reason, nowSeconds});
      }
      repository_.dequeue(flow->stationId, flow->chargerType, flowNo);
      updated.chargerId.reset();
      updated.chargerCode.reset();
    }
    repository_.saveFlow(updated);
    repository_.addFlowEvent(FlowEvent{flowNo, flow->status, updated.status,
                                       reason, nowSeconds});
    if (const auto order = repository_.orderByFlow(flowNo)) {
      if (isActiveFlowStatus(order->status)) {
        ChargingOrder cancelled = *order;
        cancelled.status = static_cast<int>(FlowStatus::Cancelled);
        repository_.saveOrder(cancelled);
      }
    }
    walletMirror_.setActiveFlowFlag(flow->userId, false);
    if (flow->chargerId) {
      promoteQueueLocked(flow->stationId, flow->chargerType, now);
    }
    view = toFlowView(updated, std::nullopt);
  });
  if (error != core::domain::ErrorCode::Ok)
    return {error, std::nullopt};
  return {core::domain::ErrorCode::Ok, *view};
}

ServiceResult<SettlementReceipt> ChargeFlowService::adminControlledSettle(
    const std::string &flowNo, const std::string &reason,
    const int nextChargerStatus,
    const std::chrono::system_clock::time_point now) {
  if (!validReasonText(reason) ||
      (nextChargerStatus != 0 && nextChargerStatus != 4))
    return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
  const auto nowSeconds = unixSeconds(now);
  core::domain::ErrorCode error = core::domain::ErrorCode::Ok;
  std::optional<SettlementReceipt> receipt;
  repository_.withTransaction([&] {
    const auto flow = repository_.flow(flowNo);
    if (!flow) {
      error = core::domain::ErrorCode::NotFound;
      return;
    }
    const bool settleable =
        flow->status == static_cast<int>(FlowStatus::Charging) ||
        flow->status == static_cast<int>(FlowStatus::SettlementFailed);
    if (!settleable) {
      error = core::domain::ErrorCode::InvalidStateTransition;
      return;
    }
    const auto order = repository_.orderByFlow(flowNo);
    if (!order || !order->startedAt) {
      error = core::domain::ErrorCode::InvalidStateTransition;
      return;
    }

    ChargingFlow settling = *flow;
    settling.status = static_cast<int>(FlowStatus::Settling);
    repository_.saveFlow(settling);

    const std::int64_t simulatedSeconds = std::max<std::int64_t>(
        0, (nowSeconds - *order->startedAt) * order->timeScale);
    const std::int64_t energyMwh =
        energyMwhForDuration(order->powerWatt, simulatedSeconds);
    const int totalPrice =
        order->electricityPriceCentPerKwh + order->servicePriceCentPerKwh;
    const std::int64_t amountCent = amountCentForEnergy(energyMwh, totalPrice);

    WalletAccount wallet = repository_.wallet(flow->userId);
    const std::int64_t paidCent = std::min(wallet.balanceCent, amountCent);
    const std::int64_t debtAddedCent = amountCent - paidCent;
    wallet.balanceCent -= paidCent;
    wallet.debtCent += debtAddedCent;
    ++wallet.version;
    wallet.updatedAt = nowSeconds;

    WalletTransaction transaction;
    transaction.userId = flow->userId;
    transaction.transactionNo = numbers_.next("WT", now);
    transaction.type = WalletTransactionType::Charge;
    transaction.amountCent = -paidCent;
    transaction.balanceAfterCent = wallet.balanceCent;
    transaction.debtAfterCent = wallet.debtCent;
    transaction.relatedNo = order->orderNo;
    transaction.createdAt = nowSeconds;

    ChargingOrder settledOrder = *order;
    settledOrder.status = static_cast<int>(FlowStatus::Completed);
    settledOrder.endedAt = nowSeconds;
    settledOrder.energyMwh = energyMwh;
    settledOrder.amountCent = amountCent;
    settledOrder.paidCent = paidCent;
    settledOrder.debtAddedCent = debtAddedCent;
    settledOrder.balanceAfterCent = wallet.balanceCent;
    settledOrder.debtAfterCent = wallet.debtCent;
    settledOrder.settledAt = nowSeconds;

    ChargingFlow completed = settling;
    completed.status = static_cast<int>(FlowStatus::Completed);
    ++completed.version;
    if (completed.chargerId) {
      if (auto charger = repository_.charger(*completed.chargerId)) {
        const int fromStatus = static_cast<int>(charger->status);
        charger->status = static_cast<ChargerStatus>(nextChargerStatus);
        charger->totalCount += 1;
        charger->totalMinutes += simulatedSeconds / 60;
        repository_.saveCharger(*charger);
        repository_.addChargerStatusEvent(
            ChargerStatusEvent{charger->id, charger->stationId, fromStatus,
                               nextChargerStatus, reason, nowSeconds});
      }
    }

    repository_.saveWallet(wallet);
    repository_.addWalletTransaction(transaction);
    repository_.saveOrder(settledOrder);
    repository_.saveFlow(completed);
    repository_.addFlowEvent(FlowEvent{flowNo, settling.status,
                                       completed.status, reason, nowSeconds});
    walletMirror_.applyWalletState(flow->userId, wallet.balanceCent,
                                   wallet.debtCent);
    walletMirror_.setActiveFlowFlag(flow->userId, false);
    if (completed.chargerId) {
      promoteQueueLocked(flow->stationId, flow->chargerType, now);
    }
    receipt = toReceipt(settledOrder);
  });
  if (error != core::domain::ErrorCode::Ok)
    return {error, std::nullopt};
  return {core::domain::ErrorCode::Ok, *receipt};
}

void ChargeFlowService::promoteAvailable(
    const std::int64_t stationId, const ChargerType type,
    const std::chrono::system_clock::time_point now) {
  repository_.withTransaction([&] {
    while (promoteQueueLocked(stationId, type, now).value) {
    }
  });
}

} // namespace ncs::core::application
