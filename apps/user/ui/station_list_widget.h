#pragma once

#include "../user_demo_service.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QVBoxLayout;

namespace ncs::user
{

class StationListWidget final : public QWidget
{
    Q_OBJECT

  public:
    explicit StationListWidget(const QVector<StationSummary>& stations, QWidget* parent = nullptr);

  signals:
    void stationSelected(int stationId);

  private:
    void refresh();

    QVector<StationSummary> stations_;
    QComboBox* locationBox_ = nullptr;
    QLineEdit* searchEdit_ = nullptr;
    QLabel* summary_ = nullptr;
    QVBoxLayout* cards_ = nullptr;
};

} // namespace ncs::user
