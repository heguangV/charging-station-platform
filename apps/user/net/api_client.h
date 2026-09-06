#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkReply>
#include <QUrl>
#include <QUrlQuery>

#include <functional>
#include <memory>

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

    bool ok() const
    {
        return httpStatus >= 200 && httpStatus < 300 && code.isEmpty();
    }
};

struct BinaryApiReply
{
    QByteArray body;
    QByteArray contentType;
    QByteArray etag;
    QString code;
    QString message;
    int httpStatus = 0;

    bool ok() const
    {
        return httpStatus >= 200 && httpStatus < 300 && code.isEmpty();
    }
    bool notModified() const
    {
        return httpStatus == 304;
    }
};

class ApiClient final : public QObject
{
    Q_OBJECT
  public:
    using Handler = std::function<void(ApiReply)>;
    using BinaryHandler = std::function<void(BinaryApiReply)>;
    using BytesHandler = std::function<void(QByteArray, QString, int)>;

    explicit ApiClient(QUrl baseUrl, QObject* parent = nullptr);
    void setAccessToken(const QString& token);
    QString accessToken() const;
    void get(const QString& path, const QUrlQuery& query, Handler handler);
    void getBinary(const QString& path, const QHash<QByteArray, QByteArray>& headers,
                   BinaryHandler handler);
    void getBytes(const QString& path, BytesHandler handler);
    void postJson(const QString& path, const QJsonObject& body, Handler handler,
                  const QHash<QByteArray, QByteArray>& headers = {});
    void putJson(const QString& path, const QJsonObject& body, Handler handler);
    void deleteJson(const QString& path, const QJsonObject& body, Handler handler);
    void postMultipart(const QString& path, QHttpMultiPart* body, Handler handler);

    // Kept public to make the stable HTTP-envelope contract unit-testable without a server.
    static ApiReply parseEnvelope(int httpStatus, const QByteArray& payload,
                                  QNetworkReply::NetworkError transportError,
                                  const QString& transportMessage);
    static BinaryApiReply parseBinaryReply(int httpStatus, const QByteArray& payload,
                                           const QByteArray& contentType, const QByteArray& etag,
                                           QNetworkReply::NetworkError transportError,
                                           const QString& transportMessage);
    static bool isRetryableTransportError(QNetworkReply::NetworkError error);

  private:
    struct PendingRequest;
    struct PendingBinaryRequest;
    void send(const std::shared_ptr<PendingRequest>& pending);
    void sendBinary(const std::shared_ptr<PendingBinaryRequest>& pending);
    void enqueue(const QByteArray& method, const QString& path, const QByteArray& body,
                 QHash<QByteArray, QByteArray> headers, Handler handler,
                 QHttpMultiPart* multipart = nullptr);
    QUrl urlFor(const QString& path) const;

    QUrl baseUrl_;
    QString accessToken_;
    QNetworkAccessManager* manager_ = nullptr;
};

} // namespace ncs::user
