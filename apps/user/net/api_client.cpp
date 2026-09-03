#include "api_client.h"

#include <QHttpMultiPart>
#include <QJsonDocument>
#include <QJsonParseError>
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

struct ApiClient::PendingRequest
{
    QByteArray method;
    QString path;
    QByteArray body;
    QHash<QByteArray, QByteArray> headers;
    Handler handler;
    QHttpMultiPart* multipart = nullptr;
    int attempts = 0;
};

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
    enqueue("GET", target.toString(), {}, {}, std::move(handler));
}

void ApiClient::postJson(const QString& path, const QJsonObject& body, Handler handler,
                         const QHash<QByteArray, QByteArray>& headers)
{
    QHash<QByteArray, QByteArray> requestHeaders = headers;
    requestHeaders.insert("Content-Type", "application/json");
    enqueue("POST", path, QJsonDocument(body).toJson(QJsonDocument::Compact), requestHeaders,
            std::move(handler));
}

void ApiClient::putJson(const QString& path, const QJsonObject& body, Handler handler)
{
    enqueue("PUT", path, QJsonDocument(body).toJson(QJsonDocument::Compact),
            {{"Content-Type", "application/json"}}, std::move(handler));
}

void ApiClient::postMultipart(const QString& path, QHttpMultiPart* body, Handler handler)
{
    enqueue("POST", path, {}, {}, std::move(handler), body);
}

void ApiClient::enqueue(const QByteArray& method, const QString& path, const QByteArray& body,
                        QHash<QByteArray, QByteArray> headers, Handler handler,
                        QHttpMultiPart* multipart)
{
    headers.insert("X-Request-ID", QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8());
    auto pending = std::make_shared<PendingRequest>(
        PendingRequest{method, path, body, std::move(headers), std::move(handler), multipart, 0});
    send(pending);
}

void ApiClient::send(const std::shared_ptr<PendingRequest>& pending)
{
    QNetworkRequest request(urlFor(pending->path));
    request.setTransferTimeout(10000);
    request.setRawHeader("Accept", "application/json");
    if (!accessToken_.isEmpty()) request.setRawHeader("Authorization", "Bearer " + accessToken_.toUtf8());
    for (auto it = pending->headers.cbegin(); it != pending->headers.cend(); ++it)
        request.setRawHeader(it.key(), it.value());

    QNetworkReply* reply = nullptr;
    if (pending->multipart)
    {
        reply = manager_->post(request, pending->multipart);
        pending->multipart->setParent(reply);
    }
    else if (pending->method == "GET") reply = manager_->get(request);
    else if (pending->method == "PUT") reply = manager_->put(request, pending->body);
    else reply = manager_->post(request, pending->body);

    connect(reply, &QNetworkReply::finished, this, [this, reply, pending] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QNetworkReply::NetworkError transportError = reply->error();
        const ApiReply result = parseEnvelope(status, reply->readAll(), transportError,
                                              reply->errorString());
        reply->deleteLater();
        // A multipart device cannot be safely replayed after QNetworkReply consumes it.
        if (!pending->multipart && result.code == QStringLiteral("NetworkError") &&
            pending->attempts == 0 && isRetryableTransportError(transportError))
        {
            ++pending->attempts;
            QTimer::singleShot(250, this, [this, pending] { send(pending); });
            return;
        }
        pending->handler(result);
    });
}

ApiReply ApiClient::parseEnvelope(int httpStatus, const QByteArray& payload,
                                  QNetworkReply::NetworkError transportError,
                                  const QString& transportMessage)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (document.isObject())
    {
        const QJsonObject object = document.object();
        if (object.contains(QStringLiteral("success")))
        {
            const bool success = object.value(QStringLiteral("success")).toBool();
            const QString code = object.value(QStringLiteral("code")).toVariant().toString();
            if (success && httpStatus >= 200 && httpStatus < 300)
                return {object.value(QStringLiteral("data")), {},
                        object.value(QStringLiteral("message")).toString(), httpStatus};
            const QString userMessage = object.value(QStringLiteral("userMessage")).toString();
            return failure(httpStatus, code.isEmpty() ? QStringLiteral("HttpError") : code,
                           userMessage.isEmpty() ? object.value(QStringLiteral("message")).toString()
                                                 : userMessage);
        }
    }
    if (transportError != QNetworkReply::NoError)
    {
        const QString message = transportMessage.compare(QStringLiteral("Unknown error"), Qt::CaseInsensitive) == 0
                                    ? QString() : transportMessage;
        return failure(httpStatus, QStringLiteral("NetworkError"), message);
    }
    Q_UNUSED(parseError)
    return failure(httpStatus, QStringLiteral("InvalidResponse"), QStringLiteral("服务返回格式无效"));
}

bool ApiClient::isRetryableTransportError(QNetworkReply::NetworkError error)
{
    return error == QNetworkReply::TimeoutError || error == QNetworkReply::TemporaryNetworkFailureError ||
           error == QNetworkReply::RemoteHostClosedError || error == QNetworkReply::ConnectionRefusedError ||
           error == QNetworkReply::NetworkSessionFailedError;
}

QUrl ApiClient::urlFor(const QString& path) const
{
    const QUrl incoming(path);
    if (incoming.isValid() && !incoming.scheme().isEmpty()) return incoming;
    return baseUrl_.resolved(QUrl(path.startsWith(QLatin1Char('/')) ? path.mid(1) : path));
}

} // namespace ncs::user
