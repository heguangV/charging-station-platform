#pragma once

#include "../user_demo_service.h"

#include <QWidget>

class QVBoxLayout;

namespace ncs::user
{

class ChargerTable final : public QWidget
{
  public:
    explicit ChargerTable(QWidget* parent = nullptr);
    void setChargers(const QVector<ChargerSummary>& chargers);
    QString selectedChargerCode() const;
    qint64 selectedChargerId() const;
    int selectedChargerType() const;

  private:
    void rebuild();

    QVector<ChargerSummary> chargers_;
    QString selectedCode_;
    QVBoxLayout* cards_ = nullptr;
};

} // namespace ncs::user
