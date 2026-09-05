#pragma once

#include "../user_demo_service.h"

#include <QFrame>

class QMouseEvent;

namespace ncs::user
{

class StationCard final : public QFrame
{
    Q_OBJECT

  public:
    explicit StationCard(const StationSummary& station, QWidget* parent = nullptr);

  signals:
    void selected(int stationId);

  protected:
    void mouseReleaseEvent(QMouseEvent* event) override;

  private:
    int stationId_ = 0;
};

} // namespace ncs::user
