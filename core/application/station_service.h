#pragma once

#include "core/application/charging_repository.h"
#include "core/application/service_result.h"
#include "core/domain/error_code.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ncs::core::application {

struct ChargerView {
    std::int64_t id = 0;
    std::string code;
    ChargerType type = ChargerType::DcFast;
    std::int64_t powerWatt = 0;
    std::string connectorStandard;
    int status = 0;
    std::string statusText;
    std::int64_t totalCount = 0;
};

struct ChargerPage {
    std::vector<ChargerView> items;
    int total = 0;
    int page = 1;
    int pageSize = 20;
};

struct StationSummary {
    std::int64_t id = 0;
    std::string code;
    std::string name;
    std::string address;
    std::string adcode;
    std::int64_t latitudeE6 = 0;
    std::int64_t longitudeE6 = 0;
    int electricityPriceCentPerKwh = 0;
    int servicePriceCentPerKwh = 0;
    int totalPriceCentPerKwh = 0;
    int idleCount = 0;
    int operationalCount = 0;
    int totalCount = 0;
    std::int64_t distanceMeter = 0;
};

struct StationListResult {
    std::vector<StationSummary> items;
    int total = 0;
    int page = 1;
    int pageSize = 20;
    bool locationFallback = false;
    std::vector<ChargerType> supportedTypes;
};

struct StationDetail {
    StationSummary summary;
    std::string businessHours;
    std::vector<ChargerType> supportedTypes;
};

struct StationQuote {
    int electricityPriceCentPerKwh = 0;
    int baseServicePriceCentPerKwh = 0;
    int queueAdjustmentBp = 0;
    int mlAdjustmentBp = 0;
    int finalServicePriceCentPerKwh = 0;
    int totalPriceCentPerKwh = 0;
    std::int64_t calculatedAt = 0;
};

// Keyword-to-coordinate resolution. External map failures must degrade to the
// preset coordinates and Haversine distances, never fail the request.
class Geocoder {
public:
    struct Location {
        std::int64_t latitudeE6 = 0;
        std::int64_t longitudeE6 = 0;
    };

    virtual ~Geocoder() = default;
    virtual std::optional<Location> resolve(const std::string &keyword) = 0;
};

class StationService final {
public:
    using PriceAdjustmentLookup = std::function<std::int64_t(
        std::int64_t stationId, int chargerType, std::int64_t at)>;

    StationService(
        ChargingRepository &repository,
        Geocoder &geocoder,
        PriceAdjustmentLookup adjustmentLookup = {})
        : repository_(repository),
          geocoder_(geocoder),
          adjustmentLookup_(std::move(adjustmentLookup))
    {
    }

    StationListResult nearbyStations(
        std::optional<std::int64_t> latitudeE6,
        std::optional<std::int64_t> longitudeE6,
        const std::string &keyword,
        std::optional<ChargerType> chargerType,
        int page,
        int pageSize,
        std::chrono::system_clock::time_point now);
    ServiceResult<StationDetail> stationDetail(std::int64_t stationId, std::chrono::system_clock::time_point now);
    ServiceResult<ChargerPage> stationChargers(
        std::int64_t stationId,
        std::optional<ChargerType> type,
        std::optional<int> status,
        int page,
        int pageSize);
    ServiceResult<StationQuote> stationQuote(
        std::int64_t stationId,
        ChargerType type,
        std::chrono::system_clock::time_point now);

    static std::int64_t haversineMeter(
        std::int64_t leftLatitudeE6,
        std::int64_t leftLongitudeE6,
        std::int64_t rightLatitudeE6,
        std::int64_t rightLongitudeE6);

    static constexpr std::int64_t defaultLatitudeE6 = 39977680;
    static constexpr std::int64_t defaultLongitudeE6 = 116316417;

private:
    ChargingRepository &repository_;
    Geocoder &geocoder_;
    PriceAdjustmentLookup adjustmentLookup_;
};

} // namespace ncs::core::application
