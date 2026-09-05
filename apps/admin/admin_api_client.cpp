#include "admin_api_client.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QUuid>
#include <QUrlQuery>

namespace ncs::admin
{

namespace
{
QString extractServerMessage(const QByteArray& bodyBytes, const QString& fallback)
{
    const auto document = QJsonDocument::fromJson(bodyBytes);
    if (!document.isObject()) return fallback;
    const auto object = document.object();
    const auto userMessage = object.value(QStringLiteral("userMessage")).toString();
    if (!userMessage.isEmpty()) return userMessage;
    const auto data = object.value(QStringLiteral("data"));
    if (data.isObject()) {
        const auto dataObject = data.toObject();
        const auto nestedMessage = dataObject.value(QStringLiteral("userMessage")).toString();
        if (!nestedMessage.isEmpty()) return nestedMessage;
    }
    return fallback;
}

QString networkFailureMessage(QNetworkReply::NetworkError error, const QByteArray& bodyBytes,
                              const QString& fallback)
{
    const auto serverMessage = extractServerMessage(bodyBytes, {});
    if (!serverMessage.isEmpty()) return serverMessage;

    switch (error) {
    case QNetworkReply::ConnectionRefusedError:
        return QStringLiteral("连接被拒绝，请检查后端服务是否已启动");
    case QNetworkReply::HostNotFoundError:
        return QStringLiteral("找不到服务器地址，请检查后端配置");
    case QNetworkReply::TimeoutError:
        return QStringLiteral("请求超时，请稍后重试");
    case QNetworkReply::SslHandshakeFailedError:
        return QStringLiteral("SSL 握手失败，请检查证书配置");
    default:
        return fallback;
    }
}

QByteArray uuidHeaderValue()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8();
}
} // namespace

AdminApiClient::AdminApiClient(QObject* parent)
    : QObject(parent), manager_(new QNetworkAccessManager(this)),
      baseUrl_(QStringLiteral("https://127.0.0.1:8443/api/v1/"))
{
}

void AdminApiClient::setBaseUrl(const QUrl& baseUrl)
{
    baseUrl_ = baseUrl;
}

void AdminApiClient::setAccessToken(const QString& accessToken)
{
    accessToken_ = accessToken;
}

const QString& AdminApiClient::accessToken() const noexcept
{
    return accessToken_;
}

QNetworkRequest AdminApiClient::buildRequest(const QString& relativePath,
                                             bool idempotent) const
{
    QNetworkRequest request(baseUrl_.resolved(QUrl(relativePath)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("X-Request-ID", uuidHeaderValue());
    if (idempotent) request.setRawHeader("Idempotency-Key", uuidHeaderValue());
    if (!accessToken_.isEmpty()) {
        request.setRawHeader("Authorization",
                             QStringLiteral("Bearer %1").arg(accessToken_).toUtf8());
    }
    return request;
}

QNetworkReply* AdminApiClient::sendJson(const QString& relativePath, const QByteArray& method,
                                        const QJsonObject& body, bool idempotent)
{
    const auto request = buildRequest(relativePath, idempotent);
    if (method == "POST") return manager_->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    if (method == "PUT") return manager_->put(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    return nullptr;
}

QNetworkReply* AdminApiClient::get(const QString& relativePath, const QUrlQuery& query)
{
    QUrl url = baseUrl_.resolved(QUrl(relativePath));
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setRawHeader("X-Request-ID", uuidHeaderValue());
    if (!accessToken_.isEmpty()) request.setRawHeader("Authorization",
                                                     QStringLiteral("Bearer %1").arg(accessToken_).toUtf8());
    return manager_->get(request);
}

QNetworkReply* AdminApiClient::postJson(const QString& relativePath, const QJsonObject& body,
                                       bool idempotent)
{
    return sendJson(relativePath, QByteArrayLiteral("POST"), body, idempotent);
}

QNetworkReply* AdminApiClient::putJson(const QString& relativePath, const QJsonObject& body,
                                      bool idempotent)
{
    return sendJson(relativePath, QByteArrayLiteral("PUT"), body, idempotent);
}

void AdminApiClient::login(const QString& username, const QString& password, const QString& deviceId)
{
    QJsonObject body;
    body.insert(QStringLiteral("username"), username);
    body.insert(QStringLiteral("password"), password);
    body.insert(QStringLiteral("deviceId"), deviceId);

    auto* reply = postJson(QStringLiteral("admin/auth/login"), body);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const auto bodyBytes = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit requestFailed(
                networkFailureMessage(reply->error(), bodyBytes, reply->errorString()));
            reply->deleteLater();
            return;
        }

        const auto document = QJsonDocument::fromJson(bodyBytes);
        if (!document.isObject()) {
            emit requestFailed(QStringLiteral("管理员登录失败：响应格式错误"));
            reply->deleteLater();
            return;
        }

        const auto object = document.object();
        const auto code = object.value(QStringLiteral("code")).toInt(0);
        const auto success = object.value(QStringLiteral("success")).toBool(code == 0);
        if (!success || code != 0) {
            emit requestFailed(object.value(QStringLiteral("userMessage")).toString(
                QStringLiteral("管理员登录失败")));
            reply->deleteLater();
            return;
        }

        const auto token =
            object.value(QStringLiteral("data")).toObject().value(QStringLiteral("accessToken")).toString();
        if (token.isEmpty()) {
            emit requestFailed(QStringLiteral("登录响应缺少 accessToken"));
        } else {
            accessToken_ = token;
            emit loginSucceeded(token);
        }
        reply->deleteLater();
    });
}

} // namespace ncs::admin
