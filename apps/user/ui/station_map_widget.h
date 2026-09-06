#pragma once

#include "../user_demo_service.h"

#include <QWidget>

#include <functional>

class QMouseEvent;
class QPaintEvent;

namespace ncs::user
{

// 基于后端坐标的示意选站图：把站点投影到画布，空闲数上色，点击标记选中站点。
class StationMapWidget final : public QWidget
{
  public:
    explicit StationMapWidget(QWidget* parent = nullptr);

    void setStations(QVector<StationSummary> stations);
    void setOnStationSelected(std::function<void(int)> callback);

  protected:
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

  private:
    QPointF pointFor(const StationSummary& station) const;

    QVector<StationSummary> stations_;
    std::function<void(int)> onStationSelected_;
};

} // namespace ncs::user
