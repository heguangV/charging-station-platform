#include "infrastructure/map/tencent_map_client.h"

#include <QEventLoop>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <utility>

namespace ncs::infrastructure::map
{

TencentMapClient::TencentMapClient(QString serverKey, const int timeoutMs)
    : serverKey_(std::move(serverKey)), timeoutMs_(timeoutMs > 0 ? timeoutMs : defaultTimeoutMs)
{
}

bool TencentMapClient::configured() const
{
    return !serverKey_.isEmpty() && serverKey_ != QStringLiteral("replace_me");
}

std::optional<QJsonObject> TencentMapClient::parseEnvelope(const QByteArray& payload)
{
    const auto document = QJsonDocument::fromJson(payload);
    if (!document.isObject())
        return std::nullopt;
    const auto root = document.object();
    if (root.value(QStringLiteral("status")).toInt(-1) != 0)
        return std::nullopt;
    return root;
}

std::optional<QJsonObject> TencentMapClient::get(const QString& path, const QUrlQuery& query) const
{
    if (!configured())
        return std::nullopt;

    QUrlQuery parameters(query);
    parameters.addQueryItem(QStringLiteral("key"), serverKey_);
    QUrl endpoint(QString::fromLatin1(webServiceHost) + path);
    endpoint.setQuery(parameters);

    QNetworkRequest request(endpoint);
    request.setTransferTimeout(timeoutMs_);
    QNetworkAccessManager manager;
    QEventLoop loop;
    QTimer::singleShot(timeoutMs_, &loop, &QEventLoop::quit);
    QNetworkReply* reply = manager.get(request);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    if (!reply->isFinished())
        reply->abort();
    const auto failure = reply->error();
    const auto payload = reply->readAll();
    reply->deleteLater();
    if (failure != QNetworkReply::NoError)
        return std::nullopt;
    return parseEnvelope(payload);
}

} // namespace ncs::infrastructure::map
