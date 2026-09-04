#include "core/application/pricing.h"

#include <algorithm>

namespace ncs::core::application {

PriceBreakdown computePrice(
    const RegionTariff &tariff,
    const int queueWaitingCount,
    const int approvedAdjustmentBp)
{
    PriceBreakdown breakdown;
    breakdown.electricityPriceCentPerKwh = tariff.electricityCentPerKwh;
    breakdown.baseServicePriceCentPerKwh = tariff.serviceCentPerKwh;
    breakdown.queueAdjustmentBp = std::min(5000, 1000 * std::max(0, queueWaitingCount));
    breakdown.mlAdjustmentBp = approvedAdjustmentBp;
    const int combinedBp = std::clamp(
        breakdown.queueAdjustmentBp + breakdown.mlAdjustmentBp, -2000, 4000);
    const long long scaled = static_cast<long long>(breakdown.baseServicePriceCentPerKwh)
        * (10000 + combinedBp);
    breakdown.finalServicePriceCentPerKwh = static_cast<int>((scaled + 5000) / 10000);
    breakdown.totalPriceCentPerKwh =
        breakdown.electricityPriceCentPerKwh + breakdown.finalServicePriceCentPerKwh;
    return breakdown;
}

std::int64_t amountCentForEnergy(const std::int64_t energyMwh, const int totalPriceCentPerKwh)
{
    return (energyMwh * totalPriceCentPerKwh + 500000) / 1000000;
}

// W*s*1000/3600 gives milliwatt-hours; *10/36 keeps exact integer math.
std::int64_t energyMwhForDuration(const std::int64_t powerWatt, const std::int64_t simulatedSeconds)
{
    return powerWatt * simulatedSeconds * 10 / 36;
}

} // namespace ncs::core::application
