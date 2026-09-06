#pragma once

#include "api_client.h"

#include <optional>

class QFile;

namespace ncs::user
{

// Keeps the V1 user REST contract out of Widgets and page classes.
class UserApi final
{
  public:
    explicit UserApi(ApiClient& client);

    void setAccessToken(const QString& token);

    void requestSmsCode(const QString& phone, ApiClient::Handler done);
    void requestPasswordResetCode(const QString& phone, ApiClient::Handler done);
    void loginSms(const QString& phone, const QString& smsCode, const QString& deviceId,
                  ApiClient::Handler done);
    void logout(ApiClient::Handler done);
    void currentProfile(ApiClient::Handler done);
    void avatarContent(ApiClient::BytesHandler done);
    void updateProfile(const QString& nickname, qint64 version, ApiClient::Handler done);
    void currentAvatar(const QByteArray& etag, ApiClient::BinaryHandler done);
    void uploadAvatar(QFile* image, const QString& fileName, ApiClient::Handler done);
    void uploadAvatar(const QByteArray& image, const QString& fileName,
                      const QByteArray& contentType, ApiClient::Handler done);
    void recharge(qint64 amountCent, ApiClient::Handler done);
    void orders(int page, int pageSize, ApiClient::Handler done);
    void order(const QString& orderNo, ApiClient::Handler done);
    void deleteAccount(const QString& smsCode, ApiClient::Handler done);
    void stations(qint64 latitudeE6, qint64 longitudeE6, const QString& keyword,
                  ApiClient::Handler done);
    void chargers(qint64 stationId, ApiClient::Handler done);
    void navigationRoute(qint64 stationId, std::optional<qint64> latitudeE6,
                         std::optional<qint64> longitudeE6, const QString& keyword,
                         const QString& mode, ApiClient::Handler done);
    void requestFlow(qint64 stationId, int chargerType, qint64 preferredChargerId,
                     ApiClient::Handler done);
    void activeFlow(ApiClient::Handler done);
    void flow(const QString& flowNo, ApiClient::Handler done);
    void confirmQuote(const QString& flowNo, const QString& quoteNo, qint64 flowVersion,
                      ApiClient::Handler done);
    void cancelFlow(const QString& flowNo, qint64 flowVersion, const QString& reasonCode,
                    ApiClient::Handler done);
    void startFlow(const QString& flowNo, qint64 flowVersion, ApiClient::Handler done);
    void progress(const QString& flowNo, ApiClient::Handler done);
    void settleFlow(const QString& flowNo, qint64 flowVersion, const QString& reasonCode,
                    ApiClient::Handler done);

  private:
    static QHash<QByteArray, QByteArray> idempotencyHeaders();
    ApiClient& client_;
};

} // namespace ncs::user
