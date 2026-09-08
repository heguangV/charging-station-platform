#pragma once

#include <QObject>

namespace ncs::user
{

// 内嵌腾讯地图页面经 WebChannel 回传站点点击；只暴露选站这一个能力，
// 不把主窗口的其它成员暴露给页面脚本（页面会加载 map.qq.com 的外部脚本）。
class StationMapBridge final : public QObject
{
    Q_OBJECT
  public:
    using QObject::QObject;

  signals:
    void stationClicked(int stationId);

  public slots:
    void selectStation(int stationId)
    {
        emit stationClicked(stationId);
    }
};

} // namespace ncs::user
