#include "core/application/station_service.h"

#include "core/application/pricing.h"

#include <algorithm>
#include <cmath>

namespace ncs::core::application {
namespace {

std::vector<ChargerType>
collectSupportedTypes(const std::vector<Charger> &chargers) {
  std::vector<ChargerType> types;
  for (const Charger &charger : chargers) {
    if (std::find(types.begin(), types.end(), charger.type) == types.end()) {
      types.push_back(charger.type);
    }
  }
  return types;
}

StationSummary summarize(const Station &station,
                         const std::vector<Charger> &chargers,
                         const RegionTariff &tariff,
                         std::int64_t distanceMeter) {
  StationSummary summary;
  summary.id = station.id;
  summary.code = station.code;
  summary.name = station.name;
  summary.address = station.address;
  summary.adcode = station.adcode;
  summary.latitudeE6 = station.latitudeE6;
  summary.longitudeE6 = station.longitudeE6;
  summary.electricityPriceCentPerKwh = tariff.electricityCentPerKwh;
  summary.servicePriceCentPerKwh = tariff.serviceCentPerKwh;
  summary.totalPriceCentPerKwh =
      tariff.electricityCentPerKwh + tariff.serviceCentPerKwh;
  summary.distanceMeter = distanceMeter;
  for (const Charger &charger : chargers) {
    if (charger.status == ChargerStatus::Idle)
      ++summary.idleCount;
    if (charger.status == ChargerStatus::Idle ||
        charger.status == ChargerStatus::Occupied) {
      ++summary.operationalCount;
    }
    ++summary.totalCount;
  }
  return summary;
}

} // namespace

std::int64_t StationService::haversineMeter(
    const std::int64_t leftLatitudeE6, const std::int64_t leftLongitudeE6,
    const std::int64_t rightLatitudeE6, const std::int64_t rightLongitudeE6) {
  constexpr double earthRadiusMeter = 6371000.0;
  const double latitude1 = leftLatitudeE6 / 1e6 * M_PI / 180.0;
  const double latitude2 = rightLatitudeE6 / 1e6 * M_PI / 180.0;
  const double deltaLatitude =
      (rightLatitudeE6 - leftLatitudeE6) / 1e6 * M_PI / 180.0;
  const double deltaLongitude =
      (rightLongitudeE6 - leftLongitudeE6) / 1e6 * M_PI / 180.0;
  const double a = std::sin(deltaLatitude / 2) * std::sin(deltaLatitude / 2) +
                   std::cos(latitude1) * std::cos(latitude2) *
                       std::sin(deltaLongitude / 2) *
                       std::sin(deltaLongitude / 2);
  return static_cast<std::int64_t>(
      2.0 * earthRadiusMeter * std::atan2(std::sqrt(a), std::sqrt(1.0 - a)) +
      0.5);
}

StationListResult StationService::nearbyStations(
    const std::optional<std::int64_t> latitudeE6,
    const std::optional<std::int64_t> longitudeE6, const std::string &keyword,
    const std::optional<ChargerType> chargerType, const int page,
    const int pageSize, const std::chrono::system_clock::time_point now) {
  const auto nowSeconds =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();
  std::int64_t originLatitude = defaultLatitudeE6;
  std::int64_t originLongitude = defaultLongitudeE6;
  StationListResult result;
  result.locationFallback = !latitudeE6 || !longitudeE6;
  if (latitudeE6 && longitudeE6) {
    originLatitude = *latitudeE6;
    originLongitude = *longitudeE6;
  } else if (!keyword.empty()) {
    if (const auto resolved = geocoder_.resolve(keyword)) {
      originLatitude = resolved->latitudeE6;
      originLongitude = resolved->longitudeE6;
      result.locationFallback = false;
    }
  }

  std::vector<StationSummary> summaries;
  for (const Station &station : repository_.stations()) {
    if (!station.enabled)
      continue;
    const auto stationChargers =
        repository_.chargers(station.id, chargerType, std::nullopt);
    if (chargerType && stationChargers.empty())
      continue;
    const auto tariff = repository_.effectiveTariff(station.adcode, nowSeconds);
    if (!tariff)
      continue;
    summaries.push_back(summarize(
        station, repository_.chargers(station.id, std::nullopt, std::nullopt),
        *tariff,
        haversineMeter(originLatitude, originLongitude, station.latitudeE6,
                       station.longitudeE6)));
  }
  std::sort(summaries.begin(), summaries.end(),
            [](const StationSummary &left, const StationSummary &right) {
              return left.distanceMeter < right.distanceMeter;
            });
  result.total = static_cast<int>(summaries.size());
  result.page = page;
  result.pageSize = pageSize;
  const auto first = static_cast<std::size_t>(page - 1) * pageSize;
  for (std::size_t index = first;
       index < summaries.size() &&
       result.items.size() < static_cast<std::size_t>(pageSize);
       ++index) {
    result.items.push_back(summaries[index]);
  }
  return result;
}

ServiceResult<StationDetail>
StationService::stationDetail(const std::int64_t stationId,
                              const std::chrono::system_clock::time_point now) {
  const auto station = repository_.station(stationId);
  if (!station)
    return {core::domain::ErrorCode::NotFound, std::nullopt};
  const auto nowSeconds =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();
  const auto tariff = repository_.effectiveTariff(station->adcode, nowSeconds);
  if (!tariff)
    return {core::domain::ErrorCode::NotFound, std::nullopt};
  StationDetail detail;
  detail.summary = summarize(
      *station, repository_.chargers(stationId, std::nullopt, std::nullopt),
      *tariff, 0);
  detail.businessHours = station->businessHours;
  detail.supportedTypes = collectSupportedTypes(
      repository_.chargers(stationId, std::nullopt, std::nullopt));
  return {core::domain::ErrorCode::Ok, detail};
}

ServiceResult<ChargerPage> StationService::stationChargers(
    const std::int64_t stationId, const std::optional<ChargerType> type,
    const std::optional<int> status, const int page, const int pageSize) {
  if (!repository_.station(stationId)) {
    return {core::domain::ErrorCode::NotFound, std::nullopt};
  }
  std::optional<ChargerStatus> chargerStatus;
  if (status) {
    if (*status < 0 || *status > 3) {
      return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
    }
    chargerStatus = static_cast<ChargerStatus>(*status);
  }
  const auto all = repository_.chargers(stationId, type, chargerStatus);
  ChargerPage result;
  result.total = static_cast<int>(all.size());
  result.page = page;
  result.pageSize = pageSize;
  const auto first = static_cast<std::size_t>(page - 1) * pageSize;
  for (std::size_t index = first;
       index < all.size() &&
       result.items.size() < static_cast<std::size_t>(pageSize);
       ++index) {
    const Charger &charger = all[index];
    result.items.push_back(ChargerView{
        charger.id,
        charger.code,
        charger.type,
        charger.powerWatt,
        charger.connectorStandard,
        static_cast<int>(charger.status),
        chargerStatusText(static_cast<int>(charger.status)),
        charger.totalCount,
    });
  }
  return {core::domain::ErrorCode::Ok, result};
}

ServiceResult<StationQuote>
StationService::stationQuote(const std::int64_t stationId,
                             const ChargerType type,
                             const std::chrono::system_clock::time_point now) {
  const auto station = repository_.station(stationId);
  if (!station)
    return {core::domain::ErrorCode::NotFound, std::nullopt};
  const auto nowSeconds =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();
  const auto tariff = repository_.effectiveTariff(station->adcode, nowSeconds);
  if (!tariff)
    return {core::domain::ErrorCode::NotFound, std::nullopt};
  const auto waiting = repository_.queue(stationId, type).size();
  const std::int64_t adjustmentBp =
      adjustmentLookup_ ? adjustmentLookup_(stationId, static_cast<int>(type),
                                            nowSeconds)
                        : 0;
  const PriceBreakdown breakdown = computePrice(
      *tariff, static_cast<int>(waiting), static_cast<int>(adjustmentBp));
  StationQuote quote;
  quote.electricityPriceCentPerKwh = breakdown.electricityPriceCentPerKwh;
  quote.baseServicePriceCentPerKwh = breakdown.baseServicePriceCentPerKwh;
  quote.queueAdjustmentBp = breakdown.queueAdjustmentBp;
  quote.mlAdjustmentBp = breakdown.mlAdjustmentBp;
  quote.finalServicePriceCentPerKwh = breakdown.finalServicePriceCentPerKwh;
  quote.totalPriceCentPerKwh = breakdown.totalPriceCentPerKwh;
  quote.calculatedAt = nowSeconds;
  return {core::domain::ErrorCode::Ok, quote};
}

} // namespace ncs::core::application
