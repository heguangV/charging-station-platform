#include "infrastructure/map/tencent_route_planner.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>

namespace ncs::infrastructure::map
{
namespace
{

constexpr int timeoutMs = 3000;
constexpr qint64 maximumResponseBytes = 2 * 1024 * 1024;
constexpr qsizetype maximumPolylinePoints = 2000;
constexpr qsizetype maximumSteps = 50;

QString coordinate(const core::application::RoutePoint point)
{
    return QStringLiteral("%1,%2")
        .arg(point.latitudeE6 / 1e6, 0, 'f', 6)
        .arg(point.longitudeE6 / 1e6, 0, 'f', 6);
}

QString endpointMode(const core::application::TravelMode mode)
{
    return QString::fromLatin1(core::application::travelModeName(mode).data(),
                               core::application::travelModeName(mode).size());
}

std::vector<core::application::RoutePoint> decodePolyline(const QJsonArray& encoded)
{
    std::vector<double> values;
    values.reserve(static_cast<std::size_t>(encoded.size()));
    for (const auto value : encoded)
    {
        if (!value.isDouble())
        {
            return {};
        }
        values.push_back(value.toDouble());
    }
    if (values.size() < 4 || values.size() % 2 != 0)
    {
        return {};
    }
    for (std::size_t index = 2; index < values.size(); ++index)
    {
        values[index] = values[index - 2] + values[index] / 1e6;
    }
    std::vector<core::application::RoutePoint> points;
    const auto count = std::min(values.size() / 2, static_cast<std::size_t>(maximumPolylinePoints));
    points.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        const double latitude = values[index * 2];
        const double longitude = values[index * 2 + 1];
        if (!std::isfinite(latitude) || !std::isfinite(longitude) || latitude < -90.0 ||
            latitude > 90.0 || longitude < -180.0 || longitude > 180.0)
        {
            return {};
        }
        points.push_back({static_cast<std::int64_t>(std::llround(latitude * 1e6)),
                          static_cast<std::int64_t>(std::llround(longitude * 1e6))});
    }
    return points;
}

void appendPolyline(std::vector<core::application::RoutePoint>& target, const QJsonArray& encoded)
{
    auto points = decodePolyline(encoded);
    if (!target.empty() && !points.empty() &&
        target.back().latitudeE6 == points.front().latitudeE6 &&
        target.back().longitudeE6 == points.front().longitudeE6)
    {
        points.erase(points.begin());
    }
    const auto remaining = static_cast<std::size_t>(maximumPolylinePoints) -
                           std::min(target.size(), static_cast<std::size_t>(maximumPolylinePoints));
    if (points.size() > remaining)
    {
        points.resize(remaining);
    }
    target.insert(target.end(), points.begin(), points.end());
}

void appendSteps(std::vector<core::application::RouteStep>& target, const QJsonArray& steps)
{
    for (const auto value : steps)
    {
        if (target.size() >= static_cast<std::size_t>(maximumSteps) || !value.isObject())
        {
            return;
        }
        const QJsonObject object = value.toObject();
        if (object.contains(QStringLiteral("polyline")))
        {
            continue;
        }
        const QString instruction =
            object.value(QStringLiteral("instruction")).toString().trimmed();
        if (instruction.isEmpty())
        {
            continue;
        }
        target.push_back(
            {instruction.left(300).toStdString(),
             static_cast<std::int64_t>(object.value(QStringLiteral("distance")).toDouble()),
             static_cast<std::int64_t>(object.value(QStringLiteral("duration")).toDouble() *
                                       60.0)});
    }
}

} // namespace

TencentRoutePlanner::TencentRoutePlanner(QString serverKey) : serverKey_(std::move(serverKey)) {}

std::optional<core::application::PlannedRoute>
TencentRoutePlanner::plan(const core::application::RoutePoint origin,
                          const core::application::RoutePoint destination,
                          const core::application::TravelMode mode)
{
    if (serverKey_.isEmpty())
    {
        return std::nullopt;
    }
    QUrl endpoint(
        QStringLiteral("https://apis.map.qq.com/ws/direction/v1/%1/").arg(endpointMode(mode)));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("from"), coordinate(origin));
    query.addQueryItem(QStringLiteral("to"), coordinate(destination));
    query.addQueryItem(QStringLiteral("key"), serverKey_);
    endpoint.setQuery(query);

    QNetworkRequest request(endpoint);
    request.setTransferTimeout(timeoutMs);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkAccessManager manager;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QNetworkReply* reply = manager.get(request);
    QObject::connect(reply, &QNetworkReply::downloadProgress, reply,
                     [reply](const qint64 received, const qint64)
                     {
                         if (received > maximumResponseBytes)
                         {
                             reply->abort();
                         }
                     });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();
    if (!reply->isFinished())
    {
        reply->abort();
    }
    const auto error = reply->error();
    const QByteArray payload = reply->readAll();
    reply->deleteLater();
    if (error != QNetworkReply::NoError || payload.size() > maximumResponseBytes)
    {
        return std::nullopt;
    }
    return parseResponse(payload, mode);
}

std::optional<core::application::PlannedRoute>
TencentRoutePlanner::parseResponse(const QByteArray& payload,
                                   const core::application::TravelMode mode)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
    {
        return std::nullopt;
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("status")).toInt(-1) != 0)
    {
        return std::nullopt;
    }
    const QJsonArray routes =
        root.value(QStringLiteral("result")).toObject().value(QStringLiteral("routes")).toArray();
    if (routes.isEmpty() || !routes.first().isObject())
    {
        return std::nullopt;
    }
    const QJsonObject route = routes.first().toObject();
    core::application::PlannedRoute result;
    result.distanceMeter =
        static_cast<std::int64_t>(route.value(QStringLiteral("distance")).toDouble());
    result.durationSecond =
        static_cast<std::int64_t>(route.value(QStringLiteral("duration")).toDouble() * 60.0);
    if (result.distanceMeter <= 0 || result.durationSecond <= 0)
    {
        return std::nullopt;
    }

    if (mode == core::application::TravelMode::Transit)
    {
        for (const auto stepValue : route.value(QStringLiteral("steps")).toArray())
        {
            const QJsonObject segment = stepValue.toObject();
            appendPolyline(result.polyline, segment.value(QStringLiteral("polyline")).toArray());
            appendSteps(result.steps, segment.value(QStringLiteral("steps")).toArray());
        }
    }
    else
    {
        appendPolyline(result.polyline, route.value(QStringLiteral("polyline")).toArray());
        appendSteps(result.steps, route.value(QStringLiteral("steps")).toArray());
    }
    return result;
}

} // namespace ncs::infrastructure::map
