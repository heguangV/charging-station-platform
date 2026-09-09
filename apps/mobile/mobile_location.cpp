#include "mobile_api.h"
#include "mobile_route_query.h"
#include <QBuffer>
#ifdef NCS_HAS_POSITIONING
#include <QGeoPositionInfo>
#include <QGeoPositionInfoSource>
#endif
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHttpMultiPart>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QPermissions>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QUrlQuery>
#include <QUuid>
#include <functional>

namespace ncs::mobile
{
void MobileApi::loadRoute(qint64 stationId, const QString& mode)
{
    if (!loggedIn() || stationId <= 0)
        return;
    const int serial = ++routeRequest_;
    route_.clear();
    emit routeChanged();
    const auto query = routeQuery(latitudeE6_, longitudeE6_, gpsOrigin_, locationAddress_, mode);
    get(QStringLiteral("/user/stations/%1/route?%2")
            .arg(stationId)
            .arg(query.toString(QUrl::FullyEncoded)),
        [this, serial](const QJsonObject& d)
        {
            if (serial != routeRequest_)
                return;
            route_ = d.toVariantMap();
            emit routeChanged();
        });
}
bool MobileApi::cameraPermissionGranted() const
{
    return qGuiApp->checkPermission(QCameraPermission{}) == Qt::PermissionStatus::Granted;
}
QString MobileApi::avatarCapturePath() const
{
    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
           QStringLiteral("/ncs-avatar-capture.jpg");
}
void MobileApi::requestCameraPermission()
{
    if (cameraPermissionGranted())
    {
        emit cameraPermissionResult(true);
        return;
    }
    qGuiApp->requestPermission(QCameraPermission{}, this,
                               [this] { emit cameraPermissionResult(cameraPermissionGranted()); });
}

void MobileApi::setLocation(int region, const QString& address)
{
    const qint64 lat[] = {39904200, 39990000, 39865000, 39907000, 39891000};
    const qint64 lon[] = {116407400, 116310000, 116378000, 116195000, 116657000};
    const QStringList names = {"北京中心", "中关村", "北京南站", "石景山", "通州"};
    if (region < 0 || region >= 5)
        return;
    ++locationRequest_;
    locating_ = false;
    latitudeE6_ = lat[region];
    longitudeE6_ = lon[region];
    gpsOrigin_ = false;
    locationAddress_ = address.trimmed();
    locationLabel_ = names[region] + QStringLiteral("（模拟位置）");
    emit locationChanged();
    loadStations();
}
void MobileApi::discardCapture()
{
    QFile::remove(avatarCapturePath());
}

void MobileApi::locateDevice()
{
    if (!loggedIn() || locating_)
        return;
#ifdef NCS_HAS_POSITIONING
    QLocationPermission permission;
    permission.setAccuracy(QLocationPermission::Precise);
    if (qGuiApp->checkPermission(permission) == Qt::PermissionStatus::Undetermined)
    {
        qGuiApp->requestPermission(permission, this, [this] { locateDevice(); });
        return;
    }
    if (qGuiApp->checkPermission(permission) != Qt::PermissionStatus::Granted)
    {
        setMessage(QStringLiteral("定位权限被拒绝，请允许定位或选择模拟位置"), true);
        return;
    }
    auto* source = QGeoPositionInfoSource::createDefaultSource(this);
    if (!source)
    {
        setMessage(QStringLiteral("系统定位不可用，请选择模拟位置"), true);
        return;
    }
    locating_ = true;
    emit locationChanged();
    const int request = ++locationRequest_;
    connect(source, &QGeoPositionInfoSource::positionUpdated, this,
            [this, source, request](const QGeoPositionInfo& info)
            {
                source->deleteLater();
                if (request != locationRequest_)
                    return;
                locating_ = false;
                emit locationChanged();
                const auto age = info.timestamp().secsTo(QDateTime::currentDateTimeUtc());
                if (!info.isValid() || age < -30 || age > 120 ||
                    (info.hasAttribute(QGeoPositionInfo::HorizontalAccuracy) &&
                     info.attribute(QGeoPositionInfo::HorizontalAccuracy) > 2000))
                {
                    setMessage(QStringLiteral("定位精度不足，请重试或选择模拟位置"), true);
                    return;
                }
                latitudeE6_ = qRound64(info.coordinate().latitude() * 1000000);
                longitudeE6_ = qRound64(info.coordinate().longitude() * 1000000);
                gpsOrigin_ = true;
                locationAddress_.clear();
                locationLabel_ = QStringLiteral("当前位置（系统定位）");
                emit locationChanged();
                setMessage(QStringLiteral("定位成功"));
                loadStations();
            });
    connect(source, &QGeoPositionInfoSource::errorOccurred, this,
            [this, source, request](QGeoPositionInfoSource::Error error)
            {
                if (error == QGeoPositionInfoSource::NoError)
                    return;
                source->deleteLater();
                if (request != locationRequest_)
                    return;
                locating_ = false;
                emit locationChanged();
                setMessage(QStringLiteral("系统定位超时或不可用，请重试或选择模拟位置"), true);
            });
    source->requestUpdate(10000);
#else
    setMessage(QStringLiteral("当前构建不包含系统定位"), true);
#endif
}
} // namespace ncs::mobile
