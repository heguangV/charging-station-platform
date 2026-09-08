#include "admin_api_client.h"
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QUuid>
namespace ncs::admin
{
AdminApiClient::AdminApiClient(QUrl baseUrl, QObject* parent)
    : QObject(parent), manager_(new QNetworkAccessManager(this)), baseUrl_(std::move(baseUrl))
{
    if (!baseUrl_.path().endsWith('/'))
        baseUrl_.setPath(baseUrl_.path() + '/');
}
void AdminApiClient::setAccessToken(const QString& token)
{
    clearSession();
    accessToken_ = token;
}
void AdminApiClient::clearSession()
{
    ++sessionGeneration_;
    accessToken_.clear();
    const auto requests = pending_;
    for (auto* reply : requests)
        reply->abort();
}
AdminReply AdminApiClient::parseReply(int status, QNetworkReply::NetworkError error,
                                      const QByteArray& body)
{
    AdminReply result;
    result.httpStatus = status;
    const auto doc = QJsonDocument::fromJson(body);
    const auto object = doc.object();
    if (error == QNetworkReply::NoError && status >= 200 && status < 300 &&
        object.value("success").isBool() && object.value("success").toBool() &&
        object.value("code").isDouble() && object.value("code").toInt(-1) == 0 &&
        object.contains("data"))
    {
        result.code = 0;
        result.data = object.value("data");
        return result;
    }
    result.code = object.value("code").toInt(-1);
    if (result.code == 0)
        result.code = -1;
    result.message = object.value("userMessage").toString().left(300);
    if (!result.message.isEmpty())
        return result;
    switch (error)
    {
    case QNetworkReply::ConnectionRefusedError:
        result.message = QStringLiteral("无法连接服务，请检查服务是否启动");
        break;
    case QNetworkReply::HostNotFoundError:
        result.message = QStringLiteral("无法找到服务器，请检查连接地址");
        break;
    case QNetworkReply::SslHandshakeFailedError:
        result.message = QStringLiteral("证书验证失败，请检查服务证书与受信任 CA 配置");
        break;
    case QNetworkReply::TimeoutError:
        result.message = QStringLiteral("请求超时，请重试");
        break;
    default:
        result.message = status == 401 ? QStringLiteral("登录已过期，请重新登录")
                                       : QStringLiteral("请求失败或响应格式不符合要求，请重试");
        break;
    }
    return result;
}
void AdminApiClient::send(const QByteArray& method, const QString& path, const QUrlQuery& query,
                          const QJsonObject& body, Handler done, bool idempotent)
{
    QUrl url = baseUrl_.resolved(QUrl(path));
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setTransferTimeout(10000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Accept", "application/json");
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("X-Request-ID",
                         QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8());
    if (idempotent)
        request.setRawHeader("Idempotency-Key",
                             QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8());
    const bool authenticated = hasSession();
    if (authenticated)
        request.setRawHeader("Authorization", "Bearer " + accessToken_.toUtf8());
    auto* reply = method == "GET"
                      ? manager_->get(request)
                      : manager_->sendCustomRequest(
                            request, method, QJsonDocument(body).toJson(QJsonDocument::Compact));
    const quint64 generation = sessionGeneration_;
    pending_.insert(reply);
    connect(reply, &QNetworkReply::downloadProgress, reply,
            [reply](qint64 received, qint64)
            {
                if (received > 2 * 1024 * 1024)
                    reply->abort();
            });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, path, authenticated, generation, done = std::move(done)]
            {
                pending_.remove(reply);
                const int status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const auto result = parseReply(status, reply->error(), reply->readAll());
                reply->deleteLater();
                if (generation != sessionGeneration_)
                    return;
                // A wrong confirmation password is also 401, and must not expire a valid session.
                if (status == 401 && authenticated && path != "admin/auth/reauth" &&
                    path != "admin/me/password" && path != "admin/auth/logout")
                {
                    clearSession();
                    emit sessionExpired();
                    return;
                }
                done(result);
            });
}
void AdminApiClient::get(const QString& path, const QUrlQuery& query, Handler done)
{
    send("GET", path, query, {}, std::move(done), false);
}
void AdminApiClient::postJson(const QString& path, const QJsonObject& body, Handler done,
                              bool idempotent)
{
    send("POST", path, {}, body, std::move(done), idempotent);
}
void AdminApiClient::putJson(const QString& path, const QJsonObject& body, Handler done,
                             bool idempotent)
{
    send("PUT", path, {}, body, std::move(done), idempotent);
}
} // namespace ncs::admin
