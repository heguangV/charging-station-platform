#include "admin_api_client.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace ncs::admin
{

AdminApiClient::AdminApiClient(QObject* parent)
    : QObject(parent), manager_(new QNetworkAccessManager(this)),
      baseUrl_(QStringLiteral("https://127.0.0.1:8443/api/v1/"))
{
}

void AdminApiClient::setBaseUrl(const QUrl& baseUrl)
{
    baseUrl_ = baseUrl;
}

void AdminApiClient::login(const QString& username, const QString& password)
{
    QNetworkRequest request(baseUrl_.resolved(QUrl(QStringLiteral("admin/auth/login"))));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    QJsonObject body;
    body.insert(QStringLiteral("username"), username);
    body.insert(QStringLiteral("password"), password);

    auto* reply = manager_->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const auto bodyBytes = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit requestFailed(reply->errorString());
            reply->deleteLater();
            return;
        }

        const auto document = QJsonDocument::fromJson(bodyBytes);
        const auto object = document.object();
        if (!object.value(QStringLiteral("success")).toBool()) {
            emit requestFailed(object.value(QStringLiteral("userMessage")).toString(
                QStringLiteral("管理员登录失败")));
            reply->deleteLater();
            return;
        }

        const auto token = object.value(QStringLiteral("data")).toObject().value(QStringLiteral("token")).toString();
        if (token.isEmpty()) {
            emit requestFailed(QStringLiteral("登录响应缺少 token"));
        } else {
            emit loginSucceeded(token);
        }
        reply->deleteLater();
    });
}

} // namespace ncs::admin
