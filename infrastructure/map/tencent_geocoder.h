// 腾讯地图 WebService 地理编码器：实现 core::application::Geocoder 接口，按关键字解析经纬度。
// 调用腾讯固定 HTTPS 端点（Server Key 只在服务端持有），用 QEventLoop + QTimer 超时
// 把异步 Qt Network 请求包成同步调用；任何失败都返回 nullopt，由调用方回退预设坐标与
// Haversine 距离。
#pragma once

#include "core/application/station_service.h"

#include <QString>

namespace ncs::infrastructure::map
{

// Tencent Maps WebService geocoder. Any failure — missing key, network error,
// timeout or non-zero API status — resolves to nullopt so callers fall back to
// preset coordinates and Haversine distances.
class TencentGeocoder final : public core::application::Geocoder
{
  public:
    explicit TencentGeocoder(QString serverKey);

    std::optional<Location> resolve(const std::string& keyword) override;

  private:
    QString serverKey_;
};

} // namespace ncs::infrastructure::map
