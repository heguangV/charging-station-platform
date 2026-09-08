// 用户侧站点查询应用服务：附近站点列表（距离/价格/空闲数）、站点详情、充电桩分页与站点实时价格报价。
// 依赖 ChargingRepository（站点/充电桩/资费）、Geocoder 端口（关键词转坐标）与调价查询函数。
// 约束：地理编码失败降级为默认坐标（北京）与 Haversine 距离并标记
// locationFallback，不使请求失败；价格构成由 pricing.h 计算。

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

namespace ncs::core::application
{

struct ChargerView
{
    std::int64_t id = 0;
    std::string code;
    ChargerType type = ChargerType::DcFast;
    std::int64_t powerWatt = 0;
    std::string connectorStandard;
    int status = 0;
    std::string statusText;
    std::int64_t totalCount = 0;
};

struct ChargerPage
{
    std::vector<ChargerView> items;
    int total = 0;
    int page = 1;
    int pageSize = 20;
};

struct StationSummary
{
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

struct StationListResult
{
    std::vector<StationSummary> items;
    int total = 0;
    int page = 1;
    int pageSize = 20;
    bool locationFallback = false;
    std::vector<ChargerType> supportedTypes;
};

struct StationDetail
{
    StationSummary summary;
    std::string businessHours;
    std::vector<ChargerType> supportedTypes;
};

struct StationQuote
{
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
class Geocoder
{
  public:
    struct Location
    {
        std::int64_t latitudeE6 = 0;
        std::int64_t longitudeE6 = 0;
    };

    virtual ~Geocoder() = default;
    virtual std::optional<Location> resolve(const std::string& keyword) = 0;
};

class StationService final
{
  public:
    using PriceAdjustmentLookup =
        std::function<std::int64_t(std::int64_t stationId, int chargerType, std::int64_t at)>;

    StationService(ChargingRepository& repository, Geocoder& geocoder,
                   PriceAdjustmentLookup adjustmentLookup = {})
        : repository_(repository), geocoder_(geocoder),
          adjustmentLookup_(std::move(adjustmentLookup))
    {
    }

    // 附近站点列表：仅启用且有生效价目表的站点参与，按 Haversine
    // 距离升序分页；缺坐标时用关键词地理编码， 仍失败则回退默认坐标（北京）并置
    // locationFallback=true，不使请求失败。
    StationListResult nearbyStations(std::optional<std::int64_t> latitudeE6,
                                     std::optional<std::int64_t> longitudeE6,
                                     const std::string& keyword,
                                     std::optional<ChargerType> chargerType, int page, int pageSize,
                                     std::chrono::system_clock::time_point now);
    // 站点详情：含价格、充电桩统计与支持的桩型列表；站点或生效价目表缺失返回 NotFound。
    ServiceResult<StationDetail> stationDetail(std::int64_t stationId,
                                               std::chrono::system_clock::time_point now);
    // 站点充电桩分页：status 有效域 0~3（越界返回 ValidationFailed），站点不存在返回 NotFound。
    ServiceResult<ChargerPage> stationChargers(std::int64_t stationId,
                                               std::optional<ChargerType> type,
                                               std::optional<int> status, int page, int pageSize);
    // 站点实时报价：按当前排队数与已审定调价基点计算价格构成（单位同
    // pricing.h：分/千瓦时、基点）；站点或价目表缺失返回 NotFound。
    ServiceResult<StationQuote> stationQuote(std::int64_t stationId, ChargerType type,
                                             std::chrono::system_clock::time_point now);

    // Haversine 球面距离（米）：输入为 E6 格式（1e-6 度）经纬度整数。
    static std::int64_t haversineMeter(std::int64_t leftLatitudeE6, std::int64_t leftLongitudeE6,
                                       std::int64_t rightLatitudeE6, std::int64_t rightLongitudeE6);

    static constexpr std::int64_t defaultLatitudeE6 = 39977680;
    static constexpr std::int64_t defaultLongitudeE6 = 116316417;

  private:
    ChargingRepository& repository_;
    Geocoder& geocoder_;
    PriceAdjustmentLookup adjustmentLookup_;
};

} // namespace ncs::core::application
