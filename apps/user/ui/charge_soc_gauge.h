#pragma once

#include <QWidget>

namespace ncs::user
{

class ChargeSocGauge final : public QWidget
{
  public:
    explicit ChargeSocGauge(QWidget* parent = nullptr);
    void setValue(int value);

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    int value_ = 28;
};

} // namespace ncs::user
