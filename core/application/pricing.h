#pragma once

#include "core/application/charging_repository.h"

namespace ncs::core::application {

struct PriceBreakdown {
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
PriceBreakdown computePrice(
    const RegionTariff &tariff,
    int queueWaitingCount,
    int approvedAdjustmentBp = 0);

// BR-05: amount = energy * (electricity + service), settled in integer cents.
std::int64_t amountCentForEnergy(std::int64_t energyMwh, int totalPriceCentPerKwh);

// UC-U-08: energy accumulates as power * simulated duration; the simulated
// duration scales real seconds by the charge time scale snapshot.
std::int64_t energyMwhForDuration(std::int64_t powerWatt, std::int64_t simulatedSeconds);

} // namespace ncs::core::application
