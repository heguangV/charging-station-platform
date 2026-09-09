#pragma once

#include <QUrlQuery>

namespace ncs::mobile
{
// System GPS uses WGS84; simulated locations already use the map coordinate system.
inline QUrlQuery routeQuery(qint64 latitude, qint64 longitude, bool gpsOrigin,
                            const QString& address, const QString& mode)
{
    QUrlQuery query;
    if (address.isEmpty())
    {
        query.addQueryItem("latitudeE6", QString::number(latitude));
        query.addQueryItem("longitudeE6", QString::number(longitude));
        query.addQueryItem("coordinateType", gpsOrigin ? "wgs84" : "gcj02");
    }
    else
        query.addQueryItem("keyword", address);
    query.addQueryItem("mode", mode.isEmpty() ? "driving" : mode);
    return query;
}
} // namespace ncs::mobile
