#include "station_card.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace ncs::user
{
namespace
{
QLabel* text(const QString& value, const int size, const QString& color = QStringLiteral("#25324A"))
{
    auto* result = new QLabel(value);
    result->setWordWrap(true);
    result->setStyleSheet(QStringLiteral("font-size:%1px;color:%2;").arg(size).arg(color));
    return result;
}
} // namespace

StationCard::StationCard(const StationSummary& station, QWidget* parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("card"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(7);

    auto* heading = new QHBoxLayout;
    heading->addWidget(text(station.name, 16));
    heading->addStretch();
    auto* distance = text(station.distance, 12, QStringLiteral("#0F766E"));
    distance->setStyleSheet(distance->styleSheet() + QStringLiteral("font-weight:600;"));
    heading->addWidget(distance);
    layout->addLayout(heading);
    layout->addWidget(text(station.address, 12, QStringLiteral("#667085")));

    const QString availability = QStringLiteral("%1 / %2 空闲 · %3 / 度")
                                     .arg(station.idleCount)
                                     .arg(station.totalCount)
                                     .arg(QString::number(station.priceCentPerKwh / 100.0, 'f', 2));
    auto* badge = text(availability, 13, QStringLiteral("#087443"));
    badge->setStyleSheet(badge->styleSheet() +
                         QStringLiteral("background:#E8F8EF;border-radius:8px;padding:5px 8px;"));
    layout->addWidget(badge);

    auto* open = new QPushButton(QStringLiteral("查看电桩详情"));
    open->setObjectName(QStringLiteral("primaryButton"));
    open->setMinimumHeight(36);
    layout->addWidget(open);
    connect(open, &QPushButton::clicked, this, [this, id = station.id] { emit selected(id); });
}

} // namespace ncs::user
