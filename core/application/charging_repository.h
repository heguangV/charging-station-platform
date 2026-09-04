#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ncs::core::application {

enum class ChargerType { AcSlow = 0, DcFast = 1 };
enum class ChargerStatus {
    Idle = 0,
    Occupied = 1,
    Faulty = 2,
    Disabled = 3,
    Restarting = 4,
};

enum class FlowStatus {
    Queued = 10,
    PendingQuote = 20,
    Reserved = 30,
    Charging = 40,
    Settling = 50,
    Completed = 60,
    Cancelled = 70,
    SettlementFailed = 80,
    Expired = 90,
};

bool isActiveFlowStatus(int status);
std::string flowStatusText(int status);
std::string orderStatusText(int status);
std::string chargerStatusText(int status);
std::string chargerTypeName(ChargerType type);

struct Station {
    std::int64_t id = 0;
    std::string code;
    std::string name;
    std::string address;
    std::string adcode;
    std::int64_t latitudeE6 = 0;
    std::int64_t longitudeE6 = 0;
    std::string businessHours = "00:00-24:00";
    bool enabled = true;
    std::int64_t version = 1;
};

struct Charger {
    std::int64_t id = 0;
    std::int64_t stationId = 0;
    std::string code;
    ChargerType type = ChargerType::DcFast;
    std::int64_t powerWatt = 0;
    std::string connectorStandard = "GB/T";
    ChargerStatus status = ChargerStatus::Idle;
    std::int64_t totalCount = 0;
    std::int64_t totalMinutes = 0;
    std::int64_t version = 1;
};

struct RegionTariff {
    std::string adcode;
    int electricityCentPerKwh = 0;
    int serviceCentPerKwh = 0;
    std::int64_t effectiveFrom = 0;
    std::int64_t effectiveTo = 0;
};

struct WalletAccount {
    std::int64_t userId = 0;
    std::int64_t balanceCent = 0;
    std::int64_t debtCent = 0;
    std::int64_t version = 1;
    std::int64_t updatedAt = 0;
};

enum class WalletTransactionType { Recharge, Charge, DebtRepay };
std::string walletTransactionTypeName(WalletTransactionType type);
std::optional<WalletTransactionType> walletTransactionTypeFromName(const std::string &name);

struct WalletTransaction {
    std::int64_t id = 0;
    std::int64_t userId = 0;
    std::string transactionNo;
    WalletTransactionType type = WalletTransactionType::Recharge;
    std::int64_t amountCent = 0;
    std::int64_t balanceAfterCent = 0;
    std::int64_t debtAfterCent = 0;
    std::string relatedNo;
    std::int64_t createdAt = 0;
};

struct RechargeOrder {
    std::string rechargeNo;
    std::int64_t userId = 0;
    std::int64_t requestedCent = 0;
    std::int64_t debtPaidCent = 0;
    std::int64_t balanceAddedCent = 0;
    std::int64_t balanceAfterCent = 0;
    std::int64_t debtAfterCent = 0;
    std::int64_t completedAt = 0;
};

struct FlowQuote {
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

struct ChargingFlow {
    std::string flowNo;
    std::int64_t userId = 0;
    std::int64_t stationId = 0;
    ChargerType chargerType = ChargerType::DcFast;
    std::optional<std::int64_t> chargerId;
    std::optional<std::string> chargerCode;
    int status = static_cast<int>(FlowStatus::Queued);
    std::optional<FlowQuote> quote;
    std::optional<std::int64_t> reservedUntil;
    std::optional<std::int64_t> startedAt;
    std::int64_t version = 1;
    std::int64_t createdAt = 0;
};

struct ChargingOrder {
    std::string orderNo;
    std::string flowNo;
    std::int64_t userId = 0;
    std::int64_t stationId = 0;
    std::string stationName;
    std::int64_t chargerId = 0;
    std::string chargerCode;
    ChargerType chargerType = ChargerType::DcFast;
    int electricityPriceCentPerKwh = 0;
    int servicePriceCentPerKwh = 0;
    std::int64_t powerWatt = 0;
    int timeScale = 60;
    std::optional<std::int64_t> targetAmountCent;
    int status = static_cast<int>(FlowStatus::Charging);
    std::int64_t createdAt = 0;
    std::optional<std::int64_t> startedAt;
    std::optional<std::int64_t> endedAt;
    std::int64_t energyMwh = 0;
    std::int64_t amountCent = 0;
    std::int64_t paidCent = 0;
    std::int64_t debtAddedCent = 0;
    std::int64_t balanceAfterCent = 0;
    std::int64_t debtAfterCent = 0;
    std::optional<std::int64_t> settledAt;
};

struct FlowEvent {
    std::string flowNo;
    int fromStatus = 0;
    int toStatus = 0;
    std::string reasonCode;
    std::int64_t at = 0;
};

struct ChargerStatusEvent {
    std::int64_t chargerId = 0;
    std::int64_t stationId = 0;
    int fromStatus = 0;
    int toStatus = 0;
    std::string reason;  // admin free text or flow reasonCode
    std::int64_t at = 0;
};

struct OutboxEvent {
    std::int64_t id = 0;
    std::string eventType;
    std::string aggregateType;
    std::string aggregateId;
    int fromStatus = 0;
    int toStatus = 0;
    std::string reasonCode;
    std::int64_t createdAt = 0;
    int deliveryStatus = 0;  // 0 pending / 1 delivered / 2 dead
    int deliveryAttempts = 0;
    std::int64_t availableAt = 0;
};

struct WalletMovement {
    std::int64_t balanceDeltaCent = 0;
    std::int64_t debtDeltaCent = 0;
};

// One aggregate store for the charging domain. Every service mutation runs
// inside withTransaction, which maps to a SQLite BEGIN IMMEDIATE transaction
// in the future persistent adapter; the in-memory implementation only guards
// the maps with a re-entrant mutex.
class ChargingRepository {
public:
    virtual ~ChargingRepository() = default;

    virtual void withTransaction(const std::function<void()> &work) = 0;

    virtual WalletAccount wallet(std::int64_t userId) = 0;
    virtual void saveWallet(const WalletAccount &wallet) = 0;
    virtual void addWalletTransaction(const WalletTransaction &transaction) = 0;
    virtual std::vector<WalletTransaction> walletTransactions(
        std::int64_t userId,
        std::optional<WalletTransactionType> type,
        std::int64_t fromAt,
        std::int64_t toAt) = 0;
    virtual void addRechargeOrder(const RechargeOrder &order) = 0;

    virtual std::vector<Station> stations() = 0;
    virtual std::optional<Station> station(std::int64_t stationId) = 0;
    virtual std::vector<Charger> chargers(
        std::optional<std::int64_t> stationId,
        std::optional<ChargerType> type,
        std::optional<ChargerStatus> status) = 0;
    virtual std::optional<Charger> charger(std::int64_t chargerId) = 0;
    virtual void saveCharger(const Charger &charger) = 0;
    virtual std::optional<RegionTariff> effectiveTariff(
        const std::string &adcode,
        std::int64_t at) = 0;

    virtual void addFlow(const ChargingFlow &flow) = 0;
    virtual void saveFlow(const ChargingFlow &flow) = 0;
    virtual std::optional<ChargingFlow> flow(const std::string &flowNo) = 0;
    virtual std::optional<ChargingFlow> activeFlow(std::int64_t userId) = 0;
    virtual std::vector<ChargingFlow> flowsWithStatus(int status) = 0;
    virtual void addFlowEvent(const FlowEvent &event) = 0;
    virtual void addChargerStatusEvent(const ChargerStatusEvent &event) = 0;
    virtual std::vector<OutboxEvent> pollOutbox(std::int64_t now, int limit) = 0;
    virtual void markOutboxDelivered(const std::vector<std::int64_t> &ids) = 0;
    virtual void markOutboxAttempted(const std::vector<std::int64_t> &ids) = 0;
    virtual void markOutboxDead(const std::vector<std::int64_t> &ids) = 0;

    virtual void enqueue(std::int64_t stationId, ChargerType type, const std::string &flowNo) = 0;
    virtual void dequeue(std::int64_t stationId, ChargerType type, const std::string &flowNo) = 0;
    virtual std::deque<std::string> queue(std::int64_t stationId, ChargerType type) = 0;

    virtual void addOrder(const ChargingOrder &order) = 0;
    virtual void saveOrder(const ChargingOrder &order) = 0;
    virtual std::optional<ChargingOrder> order(const std::string &orderNo) = 0;
    virtual std::optional<ChargingOrder> orderByFlow(const std::string &flowNo) = 0;
    virtual std::vector<ChargingOrder> orders(
        std::int64_t userId,
        std::optional<int> status,
        std::int64_t fromAt,
        std::int64_t toAt) = 0;

    // Admin/ops surface: station, charger and tariff provisioning plus
    // full-domain scans for oversight queries.
    virtual bool addStation(Station &station) = 0;
    virtual bool saveStation(const Station &station) = 0;
    virtual bool stationCodeExists(const std::string &code) = 0;
    virtual bool addCharger(Charger &charger) = 0;
    virtual bool chargerCodeExists(const std::string &code) = 0;
    virtual void addTariff(const RegionTariff &tariff) = 0;
    virtual std::vector<RegionTariff>
    tariffVersions(std::optional<std::string> adcode) = 0;
    virtual std::vector<ChargingFlow> allFlows() = 0;
    virtual std::vector<ChargingOrder> allOrders() = 0;
};

// In-memory adapter with demo stations; replaced by the SQLite adapter later
// without touching the service or controller layers.
class InMemoryChargingRepository final : public ChargingRepository {
public:
    InMemoryChargingRepository();

    void withTransaction(const std::function<void()> &work) override;
    WalletAccount wallet(std::int64_t userId) override;
    void saveWallet(const WalletAccount &wallet) override;
    void addWalletTransaction(const WalletTransaction &transaction) override;
    std::vector<WalletTransaction> walletTransactions(
        std::int64_t userId,
        std::optional<WalletTransactionType> type,
        std::int64_t fromAt,
        std::int64_t toAt) override;
    void addRechargeOrder(const RechargeOrder &order) override;
    std::vector<Station> stations() override;
    std::optional<Station> station(std::int64_t stationId) override;
    std::vector<Charger> chargers(
        std::optional<std::int64_t> stationId,
        std::optional<ChargerType> type,
        std::optional<ChargerStatus> status) override;
    std::optional<Charger> charger(std::int64_t chargerId) override;
    void saveCharger(const Charger &charger) override;
    std::optional<RegionTariff> effectiveTariff(const std::string &adcode, std::int64_t at) override;
    void addFlow(const ChargingFlow &flow) override;
    void saveFlow(const ChargingFlow &flow) override;
    std::optional<ChargingFlow> flow(const std::string &flowNo) override;
    std::optional<ChargingFlow> activeFlow(std::int64_t userId) override;
    std::vector<ChargingFlow> flowsWithStatus(int status) override;
    void addFlowEvent(const FlowEvent &event) override;
    void addChargerStatusEvent(const ChargerStatusEvent &event) override;
    std::vector<OutboxEvent> pollOutbox(std::int64_t now, int limit) override;
    void markOutboxDelivered(const std::vector<std::int64_t> &ids) override;
    void markOutboxAttempted(const std::vector<std::int64_t> &ids) override;
    void markOutboxDead(const std::vector<std::int64_t> &ids) override;
    void enqueue(std::int64_t stationId, ChargerType type, const std::string &flowNo) override;
    void dequeue(std::int64_t stationId, ChargerType type, const std::string &flowNo) override;
    std::deque<std::string> queue(std::int64_t stationId, ChargerType type) override;
    void addOrder(const ChargingOrder &order) override;
    void saveOrder(const ChargingOrder &order) override;
    std::optional<ChargingOrder> order(const std::string &orderNo) override;
    std::optional<ChargingOrder> orderByFlow(const std::string &flowNo) override;
    std::vector<ChargingOrder> orders(
        std::int64_t userId,
        std::optional<int> status,
        std::int64_t fromAt,
        std::int64_t toAt) override;

    bool addStation(Station &station) override;
    bool saveStation(const Station &station) override;
    bool stationCodeExists(const std::string &code) override;
    bool addCharger(Charger &charger) override;
    bool chargerCodeExists(const std::string &code) override;
    void addTariff(const RegionTariff &tariff) override;
    std::vector<RegionTariff>
    tariffVersions(std::optional<std::string> adcode) override;
    std::vector<ChargingFlow> allFlows() override;
    std::vector<ChargingOrder> allOrders() override;

private:
    void seedDemoData();

    mutable std::recursive_mutex mutex_;
    std::map<std::int64_t, WalletAccount> wallets_;
    std::vector<WalletTransaction> walletTransactions_;
    std::int64_t nextWalletTransactionId_ = 1;
    std::map<std::string, RechargeOrder> rechargeOrders_;
    std::map<std::int64_t, Station> stations_;
    std::map<std::int64_t, Charger> chargers_;
    std::vector<RegionTariff> tariffs_;
    std::int64_t nextStationId_ = 1;
    std::int64_t nextChargerId_ = 1;
    std::map<std::string, ChargingFlow> flows_;
    std::vector<FlowEvent> flowEvents_;
    std::map<std::string, ChargingOrder> orders_;
    std::map<std::int64_t, std::map<int, std::deque<std::string>>> queues_;
    std::vector<OutboxEvent> outbox_;
    std::int64_t nextOutboxId_ = 1;
};

} // namespace ncs::core::application
