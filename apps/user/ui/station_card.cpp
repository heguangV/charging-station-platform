#include "station_card.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
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

StationCard::StationCard(const StationSummary& station, QWidget* parent)
    : QFrame(parent), stationId_(station.id)
{
    setObjectName(QStringLiteral("card"));
    setCursor(Qt::PointingHandCursor);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 13);
    layout->setSpacing(8);

    auto* heading = new QHBoxLayout;
    auto* name = text(station.name, 16);
    name->setStyleSheet(name->styleSheet() + QStringLiteral("font-weight:600;"));
    heading->addWidget(name);
    heading->addStretch();
    auto* distance = text(station.distance, 12, QStringLiteral("#0F766E"));
    distance->setStyleSheet(
        distance->styleSheet() +
        QStringLiteral("background:#EDF7F4;border-radius:8px;padding:4px 6px;font-weight:600;"));
    heading->addWidget(distance);
    layout->addLayout(heading);
    layout->addWidget(text(station.address, 12, QStringLiteral("#667085")));

    auto* details = new QHBoxLayout;
    auto* availability =
        text(QStringLiteral("%1 / %2 空闲").arg(station.idleCount).arg(station.totalCount), 13,
             QStringLiteral("#087443"));
    availability->setStyleSheet(
        availability->styleSheet() +
        QStringLiteral("background:#E8F8EF;border-radius:8px;padding:5px 8px;font-weight:600;"));
    details->addWidget(availability);
    details->addSpacing(2);
    details->addWidget(text(
        QStringLiteral("¥%1 / 度").arg(QString::number(station.priceCentPerKwh / 100.0, 'f', 2)),
        13, QStringLiteral("#475467")));
    details->addStretch();
    details->addWidget(text(QStringLiteral("查看电桩  ›"), 12, QStringLiteral("#0F766E")));
    layout->addLayout(details);
}

void StationCard::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint()))
        emit selected(stationId_);
    QFrame::mouseReleaseEvent(event);
}

} // namespace ncs::user
