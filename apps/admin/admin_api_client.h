#pragma once
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkReply>
#include <QObject>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>
#include <functional>
class QNetworkAccessManager;
namespace ncs::admin
{
struct AdminReply
{
    QJsonValue data;
    QString message;
    int httpStatus = 0;
    int code = -1;
    bool ok() const
    {
        return httpStatus >= 200 && httpStatus < 300 && code == 0;
    }
};
class AdminApiClient final : public QObject
{
    Q_OBJECT
  public:
    using Handler = std::function<void(AdminReply)>;
    explicit AdminApiClient(QUrl baseUrl, QObject* parent = nullptr);
    void setAccessToken(const QString& token);
    void clearSession();
    bool hasSession() const
    {
        return !accessToken_.isEmpty();
    }
    void get(const QString& path, const QUrlQuery& query, Handler done);
    void postJson(const QString& path, const QJsonObject& body, Handler done,
                  bool idempotent = false);
    void putJson(const QString& path, const QJsonObject& body, Handler done,
                 bool idempotent = false);
    static AdminReply parseReply(int status, QNetworkReply::NetworkError error,
                                 const QByteArray& body);
  signals:
    void sessionExpired();

  private:
    void send(const QByteArray& method, const QString& path, const QUrlQuery& query,
              const QJsonObject& body, Handler done, bool idempotent);
    QNetworkAccessManager* manager_;
    QUrl baseUrl_;
    QString accessToken_;
    QSet<QNetworkReply*> pending_;
    quint64 sessionGeneration_ = 0;
};
} // namespace ncs::admin
