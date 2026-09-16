// POI（兴趣点）查询端口：按坐标与关键词/类别检索充电站或用户位置周边的餐饮、咖啡、
// 便利店和商场。具体地图厂商（腾讯地图 WebService）由 infrastructure/map 适配，
// core 与 agent 只依赖本抽象。与 station_service.h 的 Geocoder 端口同为外部地图端口，
// 保证“外部地图失败必须降级、不得让主流程失败”的既有约定。

#pragma once

#include "core/application/service_result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ncs::core::application
{

struct PoiQuery
{
    // 检索圆心，E6 格式（1e-6 度）经纬度整数，GCJ-02。
    std::int64_t latitudeE6 = 0;
    std::int64_t longitudeE6 = 0;
    // keyword 为空时实现应按 category 归一出检索词。
    std::string keyword;
    // 业务类别：餐饮、咖啡、便利店、商场；为空表示不限类别。
    std::string category;
    int radiusMeter = 2000;
    int limit = 10;
};

struct PoiItem
{
    std::string id;
    std::string name;
    std::string category;
    std::string address;
    std::string tel;
    std::int64_t latitudeE6 = 0;
    std::int64_t longitudeE6 = 0;
    // 相对检索圆心的距离（米），由地图服务返回或本地 Haversine 计算。
    std::int64_t distanceMeter = 0;
};

class PoiProvider
{
  public:
    virtual ~PoiProvider() = default;

    // 未配置地图 Server Key 时返回 false。
    virtual bool available() const = 0;

    // 外部服务不可用或无 Key 返回 ExternalServiceUnavailable，参数非法返回
    // ValidationFailed；调用方（Agent 工具）据此降级为“无 POI”结果。
    virtual ServiceResult<std::vector<PoiItem>> search(const PoiQuery& query) = 0;
};

} // namespace ncs::core::application
