#include "api_client.h"

#include <QHttpMultiPart>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUuid>

namespace ncs::user
{
namespace
{
ApiReply failure(int status, const QString& code, const QString& message)
{
    return {{}, code, message.isEmpty() ? QStringLiteral("网络服务不可用，请稍后重试") : message, status};
}
} // namespace

ApiClient::ApiClient(QUrl baseUrl, QObject* parent) : QObject(parent), baseUrl_(std::move(baseUrl))
{
    baseUrl_.setPath(baseUrl_.path().endsWith(QLatin1Char('/')) ? baseUrl_.path()
                                                                : baseUrl_.path() + QLatin1Char('/'));
    manager_ = new QNetworkAccessManager(this);
}

void ApiClient::setAccessToken(const QString& token) { accessToken_ = token; }
QString ApiClient::accessToken() const { return accessToken_; }

void ApiClient::get(const QString& path, const QUrlQuery& query, Handler handler)
{
    QUrl target = urlFor(path); target.setQuery(query);
    send("GET", target.toString(), {}, {}, std::move(handler));
}

void ApiClient::postJson(const QString& path, const QJsonObject& body, Handler handler,
                         const QHash<QByteArray, QByteArray>& headers)
{
    QHash<QByteArray, QByteArray> requestHeaders = headers;
    requestHeaders.insert("Content-Type", "application/json");
    send("POST", path, QJsonDocument(body).toJson(QJsonDocument::Compact), requestHeaders, std::move(handler));
}

void ApiClient::putJson(const QString& path, const QJsonObject& body, Handler handler)
{
    send("PUT", path, QJsonDocument(body).toJson(QJsonDocument::Compact), {{"Content-Type", "application/json"}}, std::move(handler));
}

void ApiClient::postMultipart(const QString& path, QHttpMultiPart* body, Handler handler)
{
    send("POST", path, {}, {}, std::move(handler), body);
}

void ApiClient::send(const QByteArray& method, const QString& path, const QByteArray& body,
                     const QHash<QByteArray, QByteArray>& headers, Handler handler,
                     QHttpMultiPart* multipart)
{
    QNetworkRequest request(urlFor(path));
    request.setTransferTimeout(12000);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("X-Request-ID", QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8());
    if (!accessToken_.isEmpty()) request.setRawHeader("Authorization", "Bearer " + accessToken_.toUtf8());
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) request.setRawHeader(it.key(), it.value());

    QNetworkReply* reply = nullptr;
    if (multipart) { reply = manager_->post(request, multipart); multipart->setParent(reply); }
    else if (method == "GET") reply = manager_->get(request);
    else if (method == "PUT") reply = manager_->put(request, body);
    else reply = manager_->post(request, body);

    connect(reply, &QNetworkReply::finished, this, [reply, handler = std::move(handler)] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError)
        {
            const QString errorText = reply->errorString();
            handler(failure(status, QStringLiteral("NetworkError"),
                            errorText.compare(QStringLiteral("Unknown error"), Qt::CaseInsensitive) == 0
                                ? QString() : errorText));
            reply->deleteLater();
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        if (!document.isObject()) handler(failure(status, QStringLiteral("InvalidResponse"), QStringLiteral("服务返回格式无效")));
        else
        {
            const QJsonObject object = document.object();
            if (!object.value(QStringLiteral("success")).toBool())
                handler(failure(status, object.value(QStringLiteral("code")).toVariant().toString(), object.value(QStringLiteral("userMessage")).toString()));
            else handler({object.value(QStringLiteral("data")), {}, object.value(QStringLiteral("message")).toString(), status});
        }
        reply->deleteLater();
    });
}

QUrl ApiClient::urlFor(const QString& path) const
{
    const QUrl incoming(path);
    if (incoming.isValid() && !incoming.scheme().isEmpty()) return incoming;
    return baseUrl_.resolved(QUrl(path.startsWith(QLatin1Char('/')) ? path.mid(1) : path));
}

} // namespace ncs::user
