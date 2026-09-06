#include "station_map_widget.h"

#include <QFont>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QRectF>

#include <algorithm>
#include <utility>

namespace ncs::user
{

StationMapWidget::StationMapWidget(QWidget* parent) : QWidget(parent)
{
    setMinimumHeight(300);
    setCursor(Qt::PointingHandCursor);
}

void StationMapWidget::setStations(QVector<StationSummary> stations)
{
    stations_ = std::move(stations);
    update();
}

void StationMapWidget::setOnStationSelected(std::function<void(int)> callback)
{
    onStationSelected_ = std::move(callback);
}

void StationMapWidget::mouseReleaseEvent(QMouseEvent* event)
{
    for (const StationSummary& station : stations_)
    {
        if (QLineF(event->position(), pointFor(station)).length() <= 18)
        {
            if (onStationSelected_)
                onStationSelected_(station.id);
            return;
        }
    }
}

void StationMapWidget::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), QColor(QStringLiteral("#EDF6F3")));
    painter.setPen(QPen(QColor(QStringLiteral("#D8E9E4")), 1));
    for (int x = 24; x < width(); x += 48)
        painter.drawLine(x, 0, x, height());
    for (int y = 24; y < height(); y += 48)
        painter.drawLine(0, y, width(), y);
    painter.setPen(QPen(QColor(QStringLiteral("#CCE4DD")), 7, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(QPointF(16, height() * 0.72), QPointF(width() - 16, height() * 0.27));
    painter.setPen(QPen(QColor(QStringLiteral("#B8D8CF")), 4, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(QPointF(width() * 0.18, 14), QPointF(width() * 0.72, height() - 14));
    if (stations_.isEmpty())
    {
        painter.setPen(QColor(QStringLiteral("#607362")));
        painter.drawText(rect(), Qt::AlignCenter,
                         QStringLiteral("暂无可展示的电站\n请返回列表刷新"));
        return;
    }
    for (const StationSummary& station : stations_)
    {
        const QPointF point = pointFor(station);
        painter.setPen(Qt::NoPen);
        painter.setBrush(station.idleCount > 0 ? QColor(QStringLiteral("#23794E"))
                                               : QColor(QStringLiteral("#98A2B3")));
        painter.drawEllipse(point, 11, 11);
        painter.setPen(Qt::white);
        painter.setFont(QFont(QString(), 9, QFont::Bold));
        painter.drawText(QRectF(point.x() - 10, point.y() - 8, 20, 16), Qt::AlignCenter,
                         QString::number(station.idleCount));
        painter.setPen(QColor(QStringLiteral("#243F30")));
        painter.setFont(QFont(QString(), 10));
        painter.drawText(QPointF(point.x() + 14, point.y() + 4), station.name.left(12));
    }
}

QPointF StationMapWidget::pointFor(const StationSummary& station) const
{
    if (stations_.isEmpty())
        return rect().center();
    const auto [minLat, maxLat] =
        std::minmax_element(stations_.cbegin(), stations_.cend(),
                            [](const StationSummary& left, const StationSummary& right)
                            { return left.latitude < right.latitude; });
    const auto [minLon, maxLon] =
        std::minmax_element(stations_.cbegin(), stations_.cend(),
                            [](const StationSummary& left, const StationSummary& right)
                            { return left.longitude < right.longitude; });
    const qreal latRange = qMax(0.002, maxLat->latitude - minLat->latitude);
    const qreal lonRange = qMax(0.002, maxLon->longitude - minLon->longitude);
    const QRectF canvas = rect().adjusted(30, 30, -30, -38);
    return {canvas.left() + canvas.width() * (station.longitude - minLon->longitude) / lonRange,
            canvas.top() + canvas.height() * (maxLat->latitude - station.latitude) / latRange};
}

} // namespace ncs::user
