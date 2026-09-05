#pragma once

#include <QJsonObject>
#include <QObject>
#include <QUrl>
#include <QUrlQuery>

class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;
class QUrlQuery;

namespace ncs::admin
{

class AdminApiClient final : public QObject
{
    Q_OBJECT

  public:
    explicit AdminApiClient(QObject* parent = nullptr);

    void setBaseUrl(const QUrl& baseUrl);
    void setAccessToken(const QString& accessToken);
    const QString& accessToken() const noexcept;
    void login(const QString& username, const QString& password, const QString& deviceId);
    QNetworkReply* get(const QString& relativePath, const QUrlQuery& query = {});
    QNetworkReply* postJson(const QString& relativePath, const QJsonObject& body,
                            bool idempotent = false);
    QNetworkReply* putJson(const QString& relativePath, const QJsonObject& body,
                           bool idempotent = false);

  signals:
    void loginSucceeded(const QString& token);
    void requestFailed(const QString& message);

  private:
    QNetworkRequest buildRequest(const QString& relativePath, bool idempotent) const;
    QNetworkReply* sendJson(const QString& relativePath, const QByteArray& method,
                            const QJsonObject& body, bool idempotent);
    QNetworkAccessManager* manager_;
    QUrl baseUrl_;
    QString accessToken_;
};

} // namespace ncs::admin
