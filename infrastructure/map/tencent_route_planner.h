// 腾讯地图路线规划器：实现 core::application::RoutePlanner 接口，调用腾讯固定 HTTPS 端点
// （Server Key 只在服务端持有），用 QEventLoop + QTimer 超时把异步 Qt Network 请求
// 包成同步调用。规划失败返回 nullopt，由核心层回退 routeFallback 本地直线结果，
// 绝不把失败当作成功路线。
#pragma once

#include "core/application/navigation_service.h"

#include <QByteArray>
#include <QString>

namespace ncs::infrastructure::map
{

class TencentRoutePlanner final : public core::application::RoutePlanner
{
  public:
    explicit TencentRoutePlanner(QString serverKey);

    std::optional<core::application::PlannedRoute>
    plan(core::application::RoutePoint origin, core::application::RoutePoint destination,
         core::application::TravelMode mode) override;

    std::optional<core::application::RoutePoint>
    normalizeGps(core::application::RoutePoint origin) override;
    static std::optional<core::application::RoutePoint> parseGpsResponse(const QByteArray& payload);

    static std::optional<core::application::PlannedRoute>
    parseResponse(const QByteArray& payload, core::application::TravelMode mode);

  private:
    QString serverKey_;
};

} // namespace ncs::infrastructure::map
