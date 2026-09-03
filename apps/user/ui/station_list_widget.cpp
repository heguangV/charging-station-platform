#include "station_list_widget.h"

#include "station_card.h"

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace ncs::user
{

StationListWidget::StationListWidget(const QVector<StationSummary>& stations, QWidget* parent)
    : QWidget(parent), stations_(stations)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    auto* controls = new QHBoxLayout;
    locationBox_ = new QComboBox;
    locationBox_->addItems({QStringLiteral("附近"), QStringLiteral("中关村"),
                            QStringLiteral("望京"), QStringLiteral("国贸")});
    searchEdit_ = new QLineEdit;
    searchEdit_->setPlaceholderText(QStringLiteral("搜索站名或地址"));
    auto* refreshButton = new QPushButton(QStringLiteral("刷新"));
    refreshButton->setObjectName(QStringLiteral("primaryButton"));
    refreshButton->setMinimumHeight(38);
    controls->addWidget(locationBox_);
    controls->addWidget(searchEdit_, 1);
    controls->addWidget(refreshButton);
    layout->addLayout(controls);
    summary_ = new QLabel;
    summary_->setStyleSheet(QStringLiteral("color:#667085;font-size:12px;"));
    layout->addWidget(summary_);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget;
    cards_ = new QVBoxLayout(content);
    cards_->setContentsMargins(0, 2, 0, 2);
    cards_->setSpacing(10);
    scroll->setWidget(content);
    layout->addWidget(scroll, 1);
    connect(locationBox_, &QComboBox::currentTextChanged, this, [this] { refresh(); });
    connect(searchEdit_, &QLineEdit::textChanged, this, [this] { refresh(); });
    connect(refreshButton, &QPushButton::clicked, this, &StationListWidget::refresh);
    refresh();
}

void StationListWidget::refresh()
{
    while (QLayoutItem* item = cards_->takeAt(0))
    {
        delete item->widget();
        delete item;
    }
    const QString location = locationBox_->currentText();
    const QString keyword = searchEdit_->text().trimmed();
    int count = 0;
    for (const StationSummary& station : stations_)
    {
        const bool locationMatch = location == QStringLiteral("附近") || station.name.contains(location) ||
                                   station.address.contains(location);
        const bool keywordMatch = keyword.isEmpty() || station.name.contains(keyword, Qt::CaseInsensitive) ||
                                  station.address.contains(keyword, Qt::CaseInsensitive);
        if (!locationMatch || !keywordMatch) continue;
        auto* card = new StationCard(station);
        connect(card, &StationCard::selected, this, &StationListWidget::stationSelected);
        cards_->addWidget(card);
        ++count;
    }
    if (count == 0)
    {
        auto* empty = new QLabel(QStringLiteral("暂无匹配的充电站\n请切换位置或调整搜索条件"));
        empty->setAlignment(Qt::AlignCenter);
        empty->setStyleSheet(QStringLiteral("padding:48px 12px;color:#667085;"));
        cards_->addWidget(empty);
    }
    cards_->addStretch();
    summary_->setText(QStringLiteral("%1 · 已按距离排序 · 找到 %2 个充电站").arg(location).arg(count));
}

} // namespace ncs::user
