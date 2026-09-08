#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QPointF>
#include <QString>
#include <QUrl>
#include <QUrlQuery>
#include <QtMath>
#include <optional>

namespace ncs::user
{
struct NavigationOrigin
{
    std::optional<qint64> latitudeE6;
    std::optional<qint64> longitudeE6;
    QString keyword;
};

inline NavigationOrigin navigationOriginForText(const QString& text)
{
    const QString value = text.trimmed();
    if (value == QStringLiteral("中关村（模拟位置）"))
        return {39984000, 116318600, {}};
    if (value == QStringLiteral("望京（模拟位置）"))
        return {39993300, 116473200, {}};
    if (value == QStringLiteral("国贸（模拟位置）"))
        return {39909100, 116460900, {}};
    if (value == QStringLiteral("当前位置（自动定位）"))
        return {};
    return {std::nullopt, std::nullopt, value};
}

inline bool usableNavigationPolyline(const QJsonArray& points)
{
    if (points.size() < 2)
        return false;
    QPointF first;
    bool distinct = false;
    for (qsizetype i = 0; i < points.size(); ++i)
    {
        const QJsonObject point = points[i].toObject();
        const auto lat = point.value(QStringLiteral("latitudeE6"));
        const auto lon = point.value(QStringLiteral("longitudeE6"));
        if (!lat.isDouble() || !lon.isDouble() || !qIsFinite(lat.toDouble()) ||
            !qIsFinite(lon.toDouble()) || qAbs(lat.toDouble()) > 90000000 ||
            qAbs(lon.toDouble()) > 180000000)
            return false;
        const QPointF coordinate(lon.toDouble(), lat.toDouble());
        if (i == 0)
            first = coordinate;
        else if (coordinate != first)
            distinct = true;
    }
    return distinct;
}

inline QString navigationDistance(qint64 meters)
{
    return meters < 1000 ? QStringLiteral("%1 米").arg(meters)
                         : QStringLiteral("%1 公里").arg(meters / 1000.0, 0, 'f', 1);
}
} // namespace ncs::user
