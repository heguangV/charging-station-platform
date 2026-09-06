#include "station_card.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QVBoxLayout>

namespace ncs::user
{
namespace
{
QLabel* text(const QString& value, const int size, const QString& color = QStringLiteral("#243F30"))
{
    auto* result = new QLabel(value);
    result->setWordWrap(true);
    result->setStyleSheet(QStringLiteral("font-size:%1px;color:%2;").arg(size).arg(color));
    return result;
}
} // namespace

StationCard::StationCard(const StationSummary& station, QWidget* parent)
    : QFrame(parent), stationId_(station.id)
{
    setObjectName(QStringLiteral("card"));
    setCursor(Qt::PointingHandCursor);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);
    auto* heading = new QHBoxLayout;
    heading->setSpacing(8);
    auto* name = text(station.name, 19, QStringLiteral("#1B2028"));
    name->setStyleSheet(name->styleSheet() + QStringLiteral("font-weight:700;"));
    heading->addWidget(name, 1);
    auto* distance = text(station.distance, 12, QStringLiteral("#555D67"));
    distance->setWordWrap(false);
    heading->addWidget(distance, 0, Qt::AlignTop);
    layout->addLayout(heading);
    auto* address = text(station.address, 13, QStringLiteral("#717A86"));
    layout->addWidget(address);
    auto* details = new QHBoxLayout;
    details->setSpacing(8);
    auto* price = new QLabel(QStringLiteral("<span style='font-size:14px'>¥</span>"
                                            "<span style='font-size:29px;font-weight:700'>%1</span>"
                                            "<span style='font-size:13px'> / 度</span>")
                                 .arg(QString::number(station.priceCentPerKwh / 100.0, 'f', 2)));
    price->setStyleSheet(QStringLiteral("color:#E76B13;background:transparent;"));
    details->addWidget(price);
    details->addStretch();
    auto* availability =
        text(QStringLiteral("空闲 %1 / %2").arg(station.idleCount).arg(station.totalCount), 13,
             QStringLiteral("#12754E"));
    availability->setWordWrap(false);
    availability->setStyleSheet(
        availability->styleSheet() +
        QStringLiteral("background:#EAF6EE;border-radius:6px;padding:5px 7px;font-weight:700;"));
    details->addWidget(availability, 0, Qt::AlignVCenter);
    layout->addLayout(details);
}

void StationCard::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint()))
        emit selected(stationId_);
    QFrame::mouseReleaseEvent(event);
}

} // namespace ncs::user
