// 用户侧充电流程状态机应用服务（UC-U-06/07/08/09）：编排“请求→排队→报价→确认→充电→结算”全流程，状态码
// 10/20/30/40/50/60/70/80/90。 依赖
// ChargingRepository（领域状态与事务）、UserAccountRepository、WalletMirror、BusinessNumbers 与
// pricing（计价）；结算失败转 80 幂等重试。 约束：事务边界由仓储 withTransaction 提供；报价有效期 5
// 分钟、预约保留 15 分钟；模拟充电时间倍率默认 60 倍。

#pragma once

#include "core/application/business_numbers.h"
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

namespace ncs::core::application
{

struct FlowQuoteView
{
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

struct FlowView
{
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

struct ActiveFlowView
{
    bool hasActiveFlow = false;
    std::optional<FlowView> flow;
};

struct QuoteConfirmationView
{
    std::string flowNo;
    std::string orderNo;
    int status = 30;
    std::int64_t chargerId = 0;
    std::string chargerCode;
    std::int64_t reservedUntil = 0;
    std::int64_t version = 3;
};

struct ChargeStartView
{
    std::string flowNo;
    std::string orderNo;
    int status = 40;
    std::int64_t startedAt = 0;
    std::int64_t powerWatt = 0;
    int timeScale = 60;
    std::int64_t version = 4;
};

struct ChargeProgressView
{
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

struct SettlementReceipt
{
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

struct OrderSummaryView
{
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

struct OrderPage
{
    std::vector<OrderSummaryView> items;
    int total = 0;
    int page = 1;
    int pageSize = 20;
};

// User-side charging state machine (UC-U-06/07/08/09, BR-02/03/04/12) plus the
// queue, expiry and recovery duties that the runtime tick drives (section 5.5).
class ChargeFlowService final
{
  public:
    using PriceAdjustmentLookup =
        std::function<std::int64_t(std::int64_t stationId, int chargerType, std::int64_t at)>;

    ChargeFlowService(ChargingRepository& repository, UserAccountRepository& accounts,
                      WalletMirror& walletMirror, BusinessNumbers& numbers, int chargeTimeScale,
                      PriceAdjustmentLookup adjustmentLookup = {})
        : repository_(repository), accounts_(accounts), walletMirror_(walletMirror),
          numbers_(numbers), chargeTimeScale_(chargeTimeScale),
          adjustmentLookup_(std::move(adjustmentLookup))
    {
    }

    // ---- 用户主流程：创建/排队、报价确认、开始充电、进度与结算、订单查询 ----
    // 创建充电请求（事务内）：先晋级站内排队流程再尝试分配空闲桩——分到桩生成 5 分钟报价（状态
    // 20），否则入 FIFO 队列（状态 10）； 账号无效返回 NotFound/UserFrozen，已有进行中流程
    // ActiveFlowExists，欠费或余额<5 元 DebtOutstanding/InsufficientBalance，指定桩不可用
    // AllocationConflict。
    ServiceResult<FlowView> createFlow(std::int64_t userId, std::int64_t stationId, int chargerType,
                                       std::optional<std::int64_t> preferredChargerId,
                                       std::chrono::system_clock::time_point now);
    ActiveFlowView activeFlow(std::int64_t userId, std::chrono::system_clock::time_point now);
    ServiceResult<FlowView> flowView(std::int64_t userId, const std::string& flowNo,
                                     std::chrono::system_clock::time_point now);
    // 确认报价（事务内，状态
    // 20→30）：校验版本一致且报价未过期，创建订单写入价格快照、设备保持占用并设 15 分钟预约期；
    // 报价过期返回 QuoteExpired，设备被并发释放返回 ChargerUnavailable，版本不符
    // VersionConflict，状态/报价号不符 InvalidStateTransition。
    ServiceResult<QuoteConfirmationView>
    confirmQuote(std::int64_t userId, const std::string& flowNo, const std::string& quoteNo,
                 std::int64_t flowVersion, std::chrono::system_clock::time_point now);
    // 取消流程（事务内，状态 10/20/30→70）：释放设备回
    // Idle、移出队列、作废未完成订单并触发队列晋级； 版本不符返回 VersionConflict，状态不可取消返回
    // InvalidStateTransition，取消原因码非法返回 ValidationFailed。
    ServiceResult<FlowView> cancel(std::int64_t userId, const std::string& flowNo,
                                   const std::string& reasonCode, std::int64_t flowVersion,
                                   std::chrono::system_clock::time_point now);
    // 开始充电（事务内，状态 30→40）：校验预约未过期且设备仍被占用，余额下限取 max(全市最低启动额 5
    // 元, 客户端下限)； 预约过期返回 ReservationExpired，欠费 DebtOutstanding，余额不足
    // InsufficientBalance，状态/版本不符 InvalidStateTransition/VersionConflict。
    ServiceResult<ChargeStartView> start(std::int64_t userId, const std::string& flowNo,
                                         std::int64_t flowVersion,
                                         std::optional<std::int64_t> targetAmountCent,
                                         std::optional<std::int64_t> balanceFloorCent,
                                         std::chrono::system_clock::time_point now);
    // 查询充电进度（只读，仅状态 40）：按模拟倍率换算时长，估算电量（毫瓦时）、金额（分）与模拟
    // SOC（自 20% 线性推算、封顶 100%）； 流程不存在或不属于该用户返回 NotFound，非充电中返回
    // InvalidStateTransition。
    ServiceResult<ChargeProgressView> progress(std::int64_t userId, const std::string& flowNo,
                                               std::chrono::system_clock::time_point now);
    // 结算充电订单（事务内，状态
    // 40/80→60）：计算电量与金额，余额不足部分转欠款，生成钱包流水、释放设备并触发队列晋级，返回结算凭据；
    // 事务失败时在独立事务落状态 80（版本不变，支持同键重试幂等续结）并返回 TransactionFailed。
    ServiceResult<SettlementReceipt> settle(std::int64_t userId, const std::string& flowNo,
                                            std::int64_t flowVersion, const std::string& reasonCode,
                                            std::chrono::system_clock::time_point now);
    ServiceResult<OrderPage> orders(std::int64_t userId, std::optional<int> status,
                                    std::int64_t fromAt, std::int64_t toAt, const std::string& sort,
                                    int page, int pageSize) const;
    ServiceResult<SettlementReceipt> receipt(std::int64_t userId, const std::string& orderNo) const;

    // ---- 运行时 tick：报价/预约过期、FIFO 队列晋级与重启后的镜像修复 ----
    // Runtime tick entries: expire quotes and reservations, promote the FIFO
    // queues, and repair account mirrors after a restart.
    // 运行时巡检（事务内）：将过期报价（状态 20）与过期预约（状态 30）的流程置为
    // 90，释放设备、作废订单并触发队列晋级。
    void runMaintenance(std::chrono::system_clock::time_point now);
    // 启动恢复（事务内）：为状态 10~50/80
    // 的遗留流程恢复钱包镜像“进行中”标记并返回恢复条数；充电中流程按持久化开始时间续计费。
    int recoverAtStartup(std::chrono::system_clock::time_point now);

    // ---- 管理操作（BR-11/UC-A-05）：强制释放与受控结算，角色与再认证校验由调用方负责 ----
    // Admin operations (BR-11 / UC-A-05); role and reauthentication checks
    // belong to the caller.
    // 管理端强制释放（事务内，仅状态 20/30→70）：设备置为指定状态（0/2/3/4）并版本
    // +1，作废订单并触发队列晋级； 版本不符返回 VersionConflict，原因或目标设备状态非法返回
    // ValidationFailed，状态不可释放返回 InvalidStateTransition。
    ServiceResult<FlowView> adminForceRelease(const std::string& flowNo, const std::string& reason,
                                              int nextChargerStatus, std::int64_t flowVersion,
                                              std::chrono::system_clock::time_point now);
    // 管理端受控结算（事务内，状态 40/80→60）：计费与欠款逻辑同用户结算，设备终态仅允许 0（空闲）或
    // 4（故障）； 原因或目标状态非法返回 ValidationFailed，状态不可结算返回
    // InvalidStateTransition。
    ServiceResult<SettlementReceipt>
    adminControlledSettle(const std::string& flowNo, const std::string& reason,
                          int nextChargerStatus, std::chrono::system_clock::time_point now);
    ServiceResult<SettlementReceipt>
    adminControlledSettle(const std::string& flowNo, const std::string& reason,
                          std::chrono::system_clock::time_point now)
    {
        return adminControlledSettle(flowNo, reason, static_cast<int>(ChargerStatus::Idle), now);
    }

    // 队列晋级入口（事务内）：循环执行单步晋级，直到无空闲桩或队列为空。
    void promoteAvailable(std::int64_t stationId, ChargerType type,
                          std::chrono::system_clock::time_point now);

    static constexpr std::int64_t minimumStartBalanceCent = 500;
    static constexpr int quoteValiditySec = 5 * 60;
    static constexpr int reservationValiditySec = 15 * 60;
    static constexpr std::int64_t simulatedBatteryCapacityMwh = 60000000;
    static constexpr int initialSimulatedSoc = 20;

  private:
    static bool validChargerType(int type)
    {
        return type == 0 || type == 1;
    }
    // 队列晋级单步：跳过并清理队首失效条目，为队首排队流程分配空闲桩并生成新报价（状态 10→20）；
    // 须在持有仓储事务时调用；无空闲桩或无有效价目表时不做任何变更并返回空结果。
    ServiceResult<FlowView> promoteQueueLocked(std::int64_t stationId, ChargerType type,
                                               std::chrono::system_clock::time_point now);

    ChargingRepository& repository_;
    UserAccountRepository& accounts_;
    WalletMirror& walletMirror_;
    BusinessNumbers& numbers_;
    int chargeTimeScale_ = 60;
    PriceAdjustmentLookup adjustmentLookup_;
};

} // namespace ncs::core::application
