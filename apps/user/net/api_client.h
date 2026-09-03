#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QUrl>
#include <QUrlQuery>

#include <functional>

class QHttpMultiPart;
class QNetworkAccessManager;

namespace ncs::user
{

struct ApiReply
{
    QJsonValue data;
    QString code;
    QString message;
    int httpStatus = 0;

    bool ok() const { return httpStatus >= 200 && httpStatus < 300 && code.isEmpty(); }
};

class ApiClient final : public QObject
{
    Q_OBJECT
  public:
    using Handler = std::function<void(ApiReply)>;

    explicit ApiClient(QUrl baseUrl, QObject* parent = nullptr);
    void setAccessToken(const QString& token);
    QString accessToken() const;
    void get(const QString& path, const QUrlQuery& query, Handler handler);
    void postJson(const QString& path, const QJsonObject& body, Handler handler,
                  const QHash<QByteArray, QByteArray>& headers = {});
    void putJson(const QString& path, const QJsonObject& body, Handler handler);
    void postMultipart(const QString& path, QHttpMultiPart* body, Handler handler);

  private:
    void send(const QByteArray& method, const QString& path, const QByteArray& body,
              const QHash<QByteArray, QByteArray>& headers, Handler handler,
              QHttpMultiPart* multipart = nullptr);
    QUrl urlFor(const QString& path) const;

    QUrl baseUrl_;
    QString accessToken_;
    QNetworkAccessManager* manager_ = nullptr;
};

} // namespace ncs::user
