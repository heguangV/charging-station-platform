#include "infrastructure/map/tencent_geocoder.h"

#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace ncs::infrastructure::map {
namespace {

constexpr int timeoutMs = 2000;

} // namespace

TencentGeocoder::TencentGeocoder(QString serverKey)
    : serverKey_(std::move(serverKey)) {}

std::optional<core::application::Geocoder::Location>
TencentGeocoder::resolve(const std::string &keyword) {
  if (serverKey_.isEmpty() || keyword.empty())
    return std::nullopt;
  QUrl endpoint(QStringLiteral("https://apis.map.qq.com/ws/geocoder/v1/"));
  QUrlQuery query;
  query.addQueryItem(QStringLiteral("address"),
                     QString::fromStdString(keyword));
  query.addQueryItem(QStringLiteral("key"), serverKey_);
  endpoint.setQuery(query);
  QNetworkRequest request(endpoint);
  request.setTransferTimeout(timeoutMs);
  QNetworkAccessManager manager;
  QEventLoop loop;
  QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
  QNetworkReply *reply = manager.get(request);
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
  loop.exec();
  if (!reply->isFinished())
    reply->abort();
  reply->deleteLater();
  if (reply->error() != QNetworkReply::NoError)
    return std::nullopt;
  const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
  const QJsonObject root = document.object();
  if (root.value(QStringLiteral("status")).toInt(-1) != 0)
    return std::nullopt;
  const QJsonObject location = root.value(QStringLiteral("result"))
                                   .toObject()
                                   .value(QStringLiteral("location"))
                                   .toObject();
  if (!location.contains(QStringLiteral("lat")) ||
      !location.contains(QStringLiteral("lng"))) {
    return std::nullopt;
  }
  Location resolved;
  resolved.latitudeE6 = static_cast<std::int64_t>(
      location.value(QStringLiteral("lat")).toDouble() * 1e6);
  resolved.longitudeE6 = static_cast<std::int64_t>(
      location.value(QStringLiteral("lng")).toDouble() * 1e6);
  return resolved;
}

} // namespace ncs::infrastructure::map
