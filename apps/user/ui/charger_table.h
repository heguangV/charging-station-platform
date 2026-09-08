#pragma once

#include "../user_demo_service.h"

#include <QWidget>

class QLabel;
class QPushButton;
class QVBoxLayout;

namespace ncs::user
{

// 枪桩信息卡内容：标题行（空闲统计）+ 默认折叠展示前几桩，其余收纳进“更多”。
class ChargerTable final : public QWidget
{
    Q_OBJECT
  public:
    explicit ChargerTable(QWidget* parent = nullptr);
    void setChargers(const QVector<ChargerSummary>& chargers);
    QString selectedChargerCode() const;
    qint64 selectedChargerId() const;
    int selectedChargerType() const;

  signals:
    void selectionChanged();

  private:
    void rebuild();
    void toggleSelection(const QString& code);

    QVector<ChargerSummary> chargers_;
    QString selectedCode_;
    QVBoxLayout* cards_ = nullptr;
    QLabel* selectedLabel_ = nullptr;
    QLabel* summaryLabel_ = nullptr;
    QPushButton* toggleButton_ = nullptr;
    QLabel* emptyLabel_ = nullptr;
    bool expanded_ = false;
};

} // namespace ncs::user
