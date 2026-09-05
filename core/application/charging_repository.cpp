#include "core/application/charging_repository.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace ncs::core::application {
namespace {

constexpr std::int64_t kFarFutureSeconds = 4102444800; // 2100-01-01 UTC

} // namespace

bool isActiveFlowStatus(const int status) {
  switch (status) {
  case 10:
  case 20:
  case 30:
  case 40:
  case 50:
  case 80:
    return true;
  default:
    return false;
  }
}

std::string flowStatusText(const int status) {
  switch (status) {
  case 10:
    return "排队中";
  case 20:
    return "待报价确认";
  case 30:
    return "已预约";
  case 40:
    return "充电中";
  case 50:
    return "结算中";
  case 60:
    return "已完成";
  case 70:
    return "已取消";
  case 80:
    return "结算失败";
  case 90:
    return "已过期";
  default:
    return "未知";
  }
}

std::string orderStatusText(const int status) {
  switch (status) {
  case 60:
    return "已完成";
  case 70:
    return "已取消";
  case 90:
    return "已过期";
  default:
    return "未知";
  }
}

std::string chargerStatusText(const int status) {
  switch (status) {
  case 0:
    return "空闲";
  case 1:
    return "占用";
  case 2:
    return "故障";
  case 3:
    return "停用";
  case 4:
    return "重启中";
  default:
    return "未知";
  }
}

std::string chargerTypeName(const ChargerType type) {
  return type == ChargerType::DcFast ? "DC_FAST" : "AC_SLOW";
}

std::string walletTransactionTypeName(const WalletTransactionType type) {
  switch (type) {
  case WalletTransactionType::Recharge:
    return "RECHARGE";
  case WalletTransactionType::Charge:
    return "CHARGE";
  case WalletTransactionType::DebtRepay:
    return "DEBT_REPAY";
  }
  return "RECHARGE";
}

std::optional<WalletTransactionType>
walletTransactionTypeFromName(const std::string &name) {
  if (name == "RECHARGE")
    return WalletTransactionType::Recharge;
  if (name == "CHARGE")
    return WalletTransactionType::Charge;
  if (name == "DEBT_REPAY")
    return WalletTransactionType::DebtRepay;
  return std::nullopt;
}

InMemoryChargingRepository::InMemoryChargingRepository() { seedDemoData(); }

void InMemoryChargingRepository::seedDemoData() {
  const auto addStation = [&](const char *code, const char *name,
                              const char *address, const char *adcode,
                              const std::int64_t latitudeE6,
                              const std::int64_t longitudeE6) {
    const std::int64_t id = nextStationId_++;
    stations_.emplace(
        id, Station{id, code, name, address, adcode, latitudeE6, longitudeE6});
    return id;
  };
  const auto addCharger = [&](const std::int64_t stationId, const char *code,
                              const ChargerType type,
                              const std::int64_t powerWatt) {
    const std::int64_t id = nextChargerId_++;
    chargers_.emplace(id, Charger{id, stationId, code, type, powerWatt});
    return id;
  };
  const auto addTariff = [&](const char *adcode, const int electricity,
                             const int service) {
    tariffs_.push_back(
        RegionTariff{adcode, electricity, service, 0, kFarFutureSeconds});
  };

  const std::int64_t zgc =
      addStation("ZGC", "NCS 中关村充电站", "北京市海淀区中关村大街 27 号",
                 "110108", 39977680, 116316417);
  const std::int64_t xierqi =
      addStation("XEQ", "NCS 西二旗充电站", "北京市海淀区上地信息路 2 号",
                 "110108", 40052768, 116307517);
  const std::int64_t cbd =
      addStation("CBD", "NCS 国贸充电站", "北京市朝阳区建国门外大街 1 号",
                 "110105", 39908372, 116457658);
  addTariff("110108", 85, 50);
  addTariff("110105", 92, 48);

  addCharger(zgc, "ZGC-DC-01", ChargerType::DcFast, 60000);
  addCharger(zgc, "ZGC-DC-02", ChargerType::DcFast, 60000);
  addCharger(zgc, "ZGC-DC-03", ChargerType::DcFast, 120000);
  addCharger(zgc, "ZGC-AC-01", ChargerType::AcSlow, 7000);
  addCharger(zgc, "ZGC-AC-02", ChargerType::AcSlow, 7000);
  addCharger(xierqi, "XEQ-DC-01", ChargerType::DcFast, 120000);
  addCharger(xierqi, "XEQ-AC-01", ChargerType::AcSlow, 7000);
  const std::int64_t cbdDc =
      addCharger(cbd, "CBD-DC-01", ChargerType::DcFast, 60000);
  addCharger(cbd, "CBD-AC-01", ChargerType::AcSlow, 7000);
  // One faulty DC pile keeps status filters and BR-08 exclusion observable.
  chargers_.at(cbdDc).status = ChargerStatus::Faulty;
}

void InMemoryChargingRepository::withTransaction(
    const std::function<void()> &work) {
  std::lock_guard lock(mutex_);
  const auto wallets = wallets_;
  const auto walletTransactions = walletTransactions_;
  const auto nextWalletTransactionId = nextWalletTransactionId_;
  const auto rechargeOrders = rechargeOrders_;
  const auto stations = stations_;
  const auto chargers = chargers_;
  const auto tariffs = tariffs_;
  const auto flows = flows_;
  const auto flowEvents = flowEvents_;
  const auto orders = orders_;
  const auto queues = queues_;
  try {
    work();
  } catch (...) {
    wallets_ = wallets;
    walletTransactions_ = walletTransactions;
    nextWalletTransactionId_ = nextWalletTransactionId;
    rechargeOrders_ = rechargeOrders;
    stations_ = stations;
    chargers_ = chargers;
    tariffs_ = tariffs;
    flows_ = flows;
    flowEvents_ = flowEvents;
    orders_ = orders;
    queues_ = queues;
    throw;
  }
}

WalletAccount InMemoryChargingRepository::wallet(const std::int64_t userId) {
  std::lock_guard lock(mutex_);
  const auto found = wallets_.find(userId);
  if (found != wallets_.end())
    return found->second;
  WalletAccount created;
  created.userId = userId;
  wallets_.emplace(userId, created);
  return created;
}

void InMemoryChargingRepository::saveWallet(const WalletAccount &wallet) {
  std::lock_guard lock(mutex_);
  wallets_[wallet.userId] = wallet;
}

void InMemoryChargingRepository::addWalletTransaction(
    const WalletTransaction &transaction) {
  std::lock_guard lock(mutex_);
  walletTransactions_.push_back(transaction);
  walletTransactions_.back().id = nextWalletTransactionId_++;
}

std::vector<WalletTransaction> InMemoryChargingRepository::walletTransactions(
    const std::int64_t userId, const std::optional<WalletTransactionType> type,
    const std::int64_t fromAt, const std::int64_t toAt) {
  std::lock_guard lock(mutex_);
  std::vector<WalletTransaction> result;
  for (auto iterator = walletTransactions_.rbegin();
       iterator != walletTransactions_.rend(); ++iterator) {
    if (iterator->userId != userId)
      continue;
    if (type && iterator->type != *type)
      continue;
    if (iterator->createdAt < fromAt)
      continue;
    if (toAt > 0 && iterator->createdAt > toAt)
      continue;
    result.push_back(*iterator);
  }
  return result;
}

void InMemoryChargingRepository::addRechargeOrder(const RechargeOrder &order) {
  std::lock_guard lock(mutex_);
  rechargeOrders_.emplace(order.rechargeNo, order);
}

std::vector<Station> InMemoryChargingRepository::stations() {
  std::lock_guard lock(mutex_);
  std::vector<Station> result;
  result.reserve(stations_.size());
  for (const auto &[id, station] : stations_) {
    (void)id;
    result.push_back(station);
  }
  return result;
}

std::optional<Station>
InMemoryChargingRepository::station(const std::int64_t stationId) {
  std::lock_guard lock(mutex_);
  const auto found = stations_.find(stationId);
  return found == stations_.end() ? std::nullopt
                                  : std::optional<Station>(found->second);
}

std::vector<Charger> InMemoryChargingRepository::chargers(
    const std::optional<std::int64_t> stationId,
    const std::optional<ChargerType> type,
    const std::optional<ChargerStatus> status) {
  std::lock_guard lock(mutex_);
  std::vector<Charger> result;
  for (const auto &[id, charger] : chargers_) {
    (void)id;
    if (stationId && charger.stationId != *stationId)
      continue;
    if (type && charger.type != *type)
      continue;
    if (status && charger.status != *status)
      continue;
    result.push_back(charger);
  }
  std::sort(result.begin(), result.end(),
            [](const Charger &left, const Charger &right) {
              return left.id < right.id;
            });
  return result;
}

std::optional<Charger>
InMemoryChargingRepository::charger(const std::int64_t chargerId) {
  std::lock_guard lock(mutex_);
  const auto found = chargers_.find(chargerId);
  return found == chargers_.end() ? std::nullopt
                                  : std::optional<Charger>(found->second);
}

void InMemoryChargingRepository::saveCharger(const Charger &charger) {
  std::lock_guard lock(mutex_);
  chargers_[charger.id] = charger;
}

std::optional<RegionTariff>
InMemoryChargingRepository::effectiveTariff(const std::string &adcode,
                                            const std::int64_t at) {
  std::lock_guard lock(mutex_);
  for (auto iterator = tariffs_.rbegin(); iterator != tariffs_.rend();
       ++iterator) {
    if (iterator->adcode == adcode && iterator->effectiveFrom <= at &&
        at <= iterator->effectiveTo) {
      return *iterator;
    }
  }
  return std::nullopt;
}

void InMemoryChargingRepository::addFlow(const ChargingFlow &flow) {
  std::lock_guard lock(mutex_);
  if (!flows_.emplace(flow.flowNo, flow).second) {
    throw std::runtime_error("duplicate flow number");
  }
}

void InMemoryChargingRepository::saveFlow(const ChargingFlow &flow) {
  std::lock_guard lock(mutex_);
  flows_[flow.flowNo] = flow;
}

std::optional<ChargingFlow>
InMemoryChargingRepository::flow(const std::string &flowNo) {
  std::lock_guard lock(mutex_);
  const auto found = flows_.find(flowNo);
  return found == flows_.end() ? std::nullopt
                               : std::optional<ChargingFlow>(found->second);
}

std::optional<ChargingFlow>
InMemoryChargingRepository::activeFlow(const std::int64_t userId) {
  std::lock_guard lock(mutex_);
  for (auto iterator = flows_.rbegin(); iterator != flows_.rend(); ++iterator) {
    if (iterator->second.userId == userId &&
        isActiveFlowStatus(iterator->second.status)) {
      return iterator->second;
    }
  }
  return std::nullopt;
}

std::vector<ChargingFlow>
InMemoryChargingRepository::flowsWithStatus(const int status) {
  std::lock_guard lock(mutex_);
  std::vector<ChargingFlow> result;
  for (auto iterator = flows_.begin(); iterator != flows_.end(); ++iterator) {
    if (iterator->second.status == status)
      result.push_back(iterator->second);
  }
  return result;
}

void InMemoryChargingRepository::addFlowEvent(const FlowEvent &event) {
  std::lock_guard lock(mutex_);
  flowEvents_.push_back(event);
  OutboxEvent outbox;
  outbox.id = nextOutboxId_++;
  outbox.eventType = event.toStatus == static_cast<int>(FlowStatus::Completed)
                         ? "order.settled"
                         : "flow.updated";
  outbox.aggregateType = "charging_flow";
  outbox.aggregateId = event.flowNo;
  outbox.fromStatus = event.fromStatus;
  outbox.toStatus = event.toStatus;
  outbox.reasonCode = event.reasonCode;
  outbox.createdAt = event.at;
  outbox.availableAt = event.at;
  outbox_.push_back(std::move(outbox));
}

void InMemoryChargingRepository::addChargerStatusEvent(
    const ChargerStatusEvent &event) {
  std::lock_guard lock(mutex_);
  OutboxEvent outbox;
  outbox.id = nextOutboxId_++;
  outbox.eventType = "charger.statusChanged";
  outbox.aggregateType = "charger";
  outbox.aggregateId = std::to_string(event.chargerId);
  outbox.fromStatus = event.fromStatus;
  outbox.toStatus = event.toStatus;
  outbox.reasonCode = event.reason;
  outbox.createdAt = event.at;
  outbox.availableAt = event.at;
  outbox_.push_back(std::move(outbox));
}

std::vector<OutboxEvent>
InMemoryChargingRepository::pollOutbox(const std::int64_t now, const int limit) {
  std::lock_guard lock(mutex_);
  std::vector<OutboxEvent> result;
  for (const auto &row : outbox_) {
    if (result.size() >= static_cast<std::size_t>(limit))
      break;
    if (row.deliveryStatus == 0 && row.availableAt <= now)
      result.push_back(row);
  }
  return result;
}

void InMemoryChargingRepository::markOutboxDelivered(
    const std::vector<std::int64_t> &ids) {
  std::lock_guard lock(mutex_);
  for (auto &row : outbox_) {
    if (std::find(ids.begin(), ids.end(), row.id) != ids.end()
        && row.deliveryStatus == 0) {
      row.deliveryStatus = 1;
    }
  }
}

void InMemoryChargingRepository::markOutboxAttempted(
    const std::vector<std::int64_t> &ids) {
  std::lock_guard lock(mutex_);
  const std::int64_t nowSeconds =
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  for (auto &row : outbox_) {
    if (std::find(ids.begin(), ids.end(), row.id) == ids.end())
      continue;
    row.deliveryAttempts += 1;
    // Mirror the SQLite repository: exponential backoff before the next
    // poll, ten attempts flip the row to dead.
    const std::int64_t shift =
        std::min<std::int64_t>(row.deliveryAttempts - 1, 6);
    const std::int64_t backoff =
        std::min<std::int64_t>(300, 5 * (1LL << shift));
    row.availableAt = std::max(row.availableAt, nowSeconds + backoff);
    if (row.deliveryAttempts >= 10)
      row.deliveryStatus = 2;
  }
}

void InMemoryChargingRepository::markOutboxDead(
    const std::vector<std::int64_t> &ids) {
  std::lock_guard lock(mutex_);
  for (auto &row : outbox_) {
    if (std::find(ids.begin(), ids.end(), row.id) != ids.end())
      row.deliveryStatus = 2;
  }
}

void InMemoryChargingRepository::enqueue(const std::int64_t stationId,
                                         const ChargerType type,
                                         const std::string &flowNo) {
  std::lock_guard lock(mutex_);
  queues_[stationId][static_cast<int>(type)].push_back(flowNo);
}

void InMemoryChargingRepository::dequeue(const std::int64_t stationId,
                                         const ChargerType type,
                                         const std::string &flowNo) {
  std::lock_guard lock(mutex_);
  auto &queue = queues_[stationId][static_cast<int>(type)];
  std::deque<std::string> replacement;
  for (auto &entry : queue) {
    if (entry != flowNo)
      replacement.push_back(std::move(entry));
  }
  queue.swap(replacement);
}

std::deque<std::string>
InMemoryChargingRepository::queue(const std::int64_t stationId,
                                  const ChargerType type) {
  std::lock_guard lock(mutex_);
  const auto station = queues_.find(stationId);
  if (station == queues_.end())
    return {};
  const auto typeQueue = station->second.find(static_cast<int>(type));
  return typeQueue == station->second.end() ? std::deque<std::string>{}
                                            : typeQueue->second;
}

void InMemoryChargingRepository::addOrder(const ChargingOrder &order) {
  std::lock_guard lock(mutex_);
  if (!orders_.emplace(order.orderNo, order).second) {
    throw std::runtime_error("duplicate order number");
  }
}

void InMemoryChargingRepository::saveOrder(const ChargingOrder &order) {
  std::lock_guard lock(mutex_);
  orders_[order.orderNo] = order;
}

std::optional<ChargingOrder>
InMemoryChargingRepository::order(const std::string &orderNo) {
  std::lock_guard lock(mutex_);
  const auto found = orders_.find(orderNo);
  return found == orders_.end() ? std::nullopt
                                : std::optional<ChargingOrder>(found->second);
}

std::optional<ChargingOrder>
InMemoryChargingRepository::orderByFlow(const std::string &flowNo) {
  std::lock_guard lock(mutex_);
  for (auto iterator = orders_.rbegin(); iterator != orders_.rend();
       ++iterator) {
    if (iterator->second.flowNo == flowNo)
      return iterator->second;
  }
  return std::nullopt;
}

std::vector<ChargingOrder> InMemoryChargingRepository::orders(
    const std::int64_t userId, const std::optional<int> status,
    const std::int64_t fromAt, const std::int64_t toAt) {
  std::lock_guard lock(mutex_);
  std::vector<ChargingOrder> result;
  for (auto iterator = orders_.rbegin(); iterator != orders_.rend();
       ++iterator) {
    if (iterator->second.userId != userId)
      continue;
    if (status && iterator->second.status != *status)
      continue;
    if (iterator->second.createdAt < fromAt)
      continue;
    if (toAt > 0 && iterator->second.createdAt > toAt)
      continue;
    result.push_back(iterator->second);
  }
  return result;
}

bool InMemoryChargingRepository::addStation(Station &station) {
  std::lock_guard lock(mutex_);
  if (stationCodeExists(station.code)) return false;
  if (station.id == 0) station.id = nextStationId_++;
  stations_.emplace(station.id, station);
  return true;
}

bool InMemoryChargingRepository::stationCodeExists(const std::string &code) {
  std::lock_guard lock(mutex_);
  for (const auto &[id, station] : stations_) {
    (void)id;
    if (station.code == code) return true;
  }
  return false;
}

bool InMemoryChargingRepository::saveStation(const Station &station) {
  std::lock_guard lock(mutex_);
  if (!stations_.count(station.id))
    return false;
  stations_[station.id] = station;
  return true;
}

bool InMemoryChargingRepository::addCharger(Charger &charger) {
  std::lock_guard lock(mutex_);
  if (chargerCodeExists(charger.code)) return false;
  if (charger.id == 0) charger.id = nextChargerId_++;
  chargers_.emplace(charger.id, charger);
  return true;
}

bool InMemoryChargingRepository::chargerCodeExists(const std::string &code) {
  std::lock_guard lock(mutex_);
  for (const auto &[id, charger] : chargers_) {
    (void)id;
    if (charger.code == code) return true;
  }
  return false;
}

void InMemoryChargingRepository::addTariff(const RegionTariff &tariff) {
  std::lock_guard lock(mutex_);
  tariffs_.push_back(tariff);
}

std::vector<RegionTariff> InMemoryChargingRepository::tariffVersions(
    std::optional<std::string> adcode) {
  std::lock_guard lock(mutex_);
  std::vector<RegionTariff> result;
  for (const auto &tariff : tariffs_) {
    if (adcode && tariff.adcode != *adcode) continue;
    result.push_back(tariff);
  }
  std::sort(result.begin(), result.end(),
            [](const RegionTariff &left, const RegionTariff &right) {
              return left.effectiveFrom < right.effectiveFrom;
            });
  return result;
}

std::vector<ChargingFlow> InMemoryChargingRepository::allFlows() {
  std::lock_guard lock(mutex_);
  std::vector<ChargingFlow> result;
  result.reserve(flows_.size());
  for (const auto &[flowNo, flow] : flows_) {
    (void)flowNo;
    result.push_back(flow);
  }
  return result;
}

std::vector<ChargingOrder> InMemoryChargingRepository::allOrders() {
  std::lock_guard lock(mutex_);
  std::vector<ChargingOrder> result;
  result.reserve(orders_.size());
  for (const auto &[orderNo, order] : orders_) {
    (void)orderNo;
    result.push_back(order);
  }
  return result;
}

} // namespace ncs::core::application
