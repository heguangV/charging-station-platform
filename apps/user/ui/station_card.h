#pragma once

#include "../user_demo_service.h"

#include <QFrame>

class QPushButton;

namespace ncs::user
{

class StationCard final : public QFrame
{
    Q_OBJECT

  public:
    explicit StationCard(const StationSummary& station, QWidget* parent = nullptr);

  signals:
    void selected(int stationId);
};

} // namespace ncs::user
