#pragma once
#include "admin_types.h"
#include <QList>
#include <QWidget>
#include <array>
namespace ncs::admin
{
class AdminTrendWidget final : public QWidget
{
  public:
    explicit AdminTrendWidget(QWidget* parent = nullptr);
    void setPoints(QList<RevenuePoint> points);

  protected:
    void paintEvent(QPaintEvent*) override;

  private:
    QList<RevenuePoint> points_;
};
class AdminStatusChart final : public QWidget
{
  public:
    explicit AdminStatusChart(QWidget* parent = nullptr);
    void setCounts(std::array<int, 5> counts);

  protected:
    void paintEvent(QPaintEvent*) override;

  private:
    std::array<int, 5> counts_{};
};
} // namespace ncs::admin
