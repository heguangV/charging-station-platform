// 充电计价纯函数：计算每千瓦时价格构成（电价 + 基础服务费 + 排队/审定调价）以及电量、金额换算。
// 被 charge_flow_service（报价/结算）与
// station_service（站点价格展示）调用；调价基点来自已审定的调价记录。
// 约束：全部整数运算——金额分、电量毫瓦时、调整基点；最终服务费保持基础服务费的
// 80%~140%（接口契约 7.13）。

#pragma once

#include "core/application/charging_repository.h"

namespace ncs::core::application
{

struct PriceBreakdown
{
    int electricityPriceCentPerKwh = 0;
    int baseServicePriceCentPerKwh = 0;
    int queueAdjustmentBp = 0;
    int mlAdjustmentBp = 0;
    int finalServicePriceCentPerKwh = 0;
    int totalPriceCentPerKwh = 0;
};

// Queue pressure and approved ML/admin adjustments only act on the base
// service fee; the electricity component stays at the regional tariff value.
// The combined basis-point adjustment is clamped so the final service fee
// stays within 80%..140% of the base service fee (interface contract 7.13).
PriceBreakdown computePrice(const RegionTariff& tariff, int queueWaitingCount,
                            int approvedAdjustmentBp = 0);

// 金额换算：金额（分）= 电量（毫瓦时）× 总单价（分/千瓦时）÷ 1000000，四舍五入。
// BR-05: amount = energy * (electricity + service), settled in integer cents.
std::int64_t amountCentForEnergy(std::int64_t energyMwh, int totalPriceCentPerKwh);

// 电量换算：电量（毫瓦时）= 功率（瓦）× 模拟秒数 × 10 ÷ 36（即瓦·秒 × 1000 ÷ 3600）。
// UC-U-08: energy accumulates as power * simulated duration; the simulated
// duration scales real seconds by the charge time scale snapshot.
std::int64_t energyMwhForDuration(std::int64_t powerWatt, std::int64_t simulatedSeconds);

} // namespace ncs::core::application
