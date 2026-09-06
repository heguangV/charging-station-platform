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
    layout->setSpacing(10);

    auto* heading = new QHBoxLayout;
    auto* name = text(station.name, 16);
    name->setStyleSheet(name->styleSheet() + QStringLiteral("font-weight:600;"));
    heading->addWidget(name);
    heading->addStretch();
    auto* distance = text(station.distance, 12, QStringLiteral("#667085"));
    heading->addWidget(distance);
    layout->addLayout(heading);
    layout->addWidget(text(station.address, 12, QStringLiteral("#667085")));

    auto* details = new QHBoxLayout;
    auto* price = text(QStringLiteral("¥%1").arg(QString::number(station.priceCentPerKwh / 100.0, 'f', 2)),
                       22, QStringLiteral("#182230"));
    price->setStyleSheet(price->styleSheet() + QStringLiteral("font-weight:700;"));
    details->addWidget(price);
    details->addWidget(text(QStringLiteral("/ 度"), 12, QStringLiteral("#667085")), 0, Qt::AlignBottom);
    details->addStretch();
    auto* availability = text(QStringLiteral("空闲 %1 / %2").arg(station.idleCount).arg(station.totalCount),
                              12, station.idleCount > 0 ? QStringLiteral("#087443") : QStringLiteral("#667085"));
    availability->setStyleSheet(availability->styleSheet() +
                                QStringLiteral("border-radius:6px;padding:4px 0;font-weight:600;"));
    details->addWidget(availability);
    layout->addLayout(details);
}

void StationCard::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint()))
        emit selected(stationId_);
    QFrame::mouseReleaseEvent(event);
}

} // namespace ncs::user
