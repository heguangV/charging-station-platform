#include "user_api.h"

#include <QHttpMultiPart>
#include <QHttpPart>
#include <QNetworkRequest>
#include <QUuid>

namespace ncs::user
{
namespace
{
const QString kUserBase = QStringLiteral("/api/v1/user");
}

UserApi::UserApi(ApiClient& client) : client_(client) {}

void UserApi::setAccessToken(const QString& token)
{
    client_.setAccessToken(token);
}

void UserApi::requestSmsCode(const QString& phone, ApiClient::Handler done)
{
    client_.postJson(kUserBase + "/auth/sms/code", {{"phone", phone}, {"purpose", "LOGIN"}},
                     std::move(done));
}

void UserApi::loginSms(const QString& phone, const QString& smsCode, const QString& deviceId,
                       ApiClient::Handler done)
{
    client_.postJson(kUserBase + "/auth/login/sms",
                     {{"phone", phone}, {"smsCode", smsCode}, {"deviceId", deviceId}},
                     std::move(done));
}

void UserApi::logout(ApiClient::Handler done)
{
    client_.postJson(kUserBase + "/auth/logout", {}, std::move(done));
}

void UserApi::currentProfile(ApiClient::Handler done)
{
    client_.get(kUserBase + "/me", {}, std::move(done));
}

void UserApi::updateProfile(const QString& nickname, qint64 version, ApiClient::Handler done)
{
    client_.putJson(kUserBase + "/me", {{"nickname", nickname}, {"version", version}},
                    std::move(done));
}

void UserApi::currentAvatar(const QByteArray& etag, ApiClient::BinaryHandler done)
{
    QHash<QByteArray, QByteArray> headers;
    if (!etag.isEmpty())
        headers.insert("If-None-Match", etag);
    client_.getBinary(kUserBase + "/me/avatar/content", headers, std::move(done));
}

void UserApi::uploadAvatar(const QByteArray& image, const QString& fileName,
                           const QByteArray& contentType, ApiClient::Handler done)
{
    auto* body = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    QHttpPart part;
    part.setHeader(
        QNetworkRequest::ContentDispositionHeader,
        QVariant(QStringLiteral("form-data; name=\"file\"; filename=\"%1\"").arg(fileName)));
    part.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    part.setBody(image);
    body->append(part);
    client_.postMultipart(kUserBase + "/me/avatar", body, std::move(done));
}

void UserApi::recharge(qint64 amountCent, ApiClient::Handler done)
{
    client_.postJson(kUserBase + "/wallet/recharges", {{"amountCent", amountCent}}, std::move(done),
                     idempotencyHeaders());
}

void UserApi::orders(int page, int pageSize, ApiClient::Handler done)
{
    QUrlQuery query;
    query.addQueryItem("page", QString::number(page));
    query.addQueryItem("pageSize", QString::number(pageSize));
    query.addQueryItem("sort", "-createdAt");
    client_.get(kUserBase + "/orders", query, std::move(done));
}

void UserApi::stations(qint64 latitudeE6, qint64 longitudeE6, const QString& keyword,
                       ApiClient::Handler done)
{
    QUrlQuery query;
    query.addQueryItem("latitudeE6", QString::number(latitudeE6));
    query.addQueryItem("longitudeE6", QString::number(longitudeE6));
    if (!keyword.trimmed().isEmpty())
        query.addQueryItem("keyword", keyword.trimmed());
    query.addQueryItem("page", "1");
    query.addQueryItem("pageSize", "20");
    client_.get(kUserBase + "/stations", query, std::move(done));
}

void UserApi::chargers(qint64 stationId, ApiClient::Handler done)
{
    client_.get(kUserBase + QStringLiteral("/stations/%1/chargers").arg(stationId), {},
                std::move(done));
}

void UserApi::navigationRoute(qint64 stationId, std::optional<qint64> latitudeE6,
                              std::optional<qint64> longitudeE6, const QString& keyword,
                              const QString& mode, ApiClient::Handler done)
{
    QUrlQuery query;
    if (latitudeE6 && longitudeE6)
    {
        query.addQueryItem(QStringLiteral("latitudeE6"), QString::number(*latitudeE6));
        query.addQueryItem(QStringLiteral("longitudeE6"), QString::number(*longitudeE6));
    }
    if (!keyword.trimmed().isEmpty())
        query.addQueryItem(QStringLiteral("keyword"), keyword.trimmed());
    query.addQueryItem(QStringLiteral("mode"), mode);
    client_.get(kUserBase + QStringLiteral("/stations/%1/route").arg(stationId), query,
                std::move(done));
}

void UserApi::requestFlow(qint64 stationId, int chargerType, qint64 preferredChargerId,
                          ApiClient::Handler done)
{
    QJsonObject body{{"stationId", stationId}, {"chargerType", chargerType}};
    if (preferredChargerId > 0)
        body.insert("preferredChargerId", preferredChargerId);
    else
        body.insert("preferredChargerId", QJsonValue::Null);
    client_.postJson(kUserBase + "/flows", body, std::move(done), idempotencyHeaders());
}

void UserApi::activeFlow(ApiClient::Handler done)
{
    client_.get(kUserBase + "/flows/active", {}, std::move(done));
}
void UserApi::flow(const QString& flowNo, ApiClient::Handler done)
{
    client_.get(kUserBase + QStringLiteral("/flows/%1").arg(flowNo), {}, std::move(done));
}
void UserApi::confirmQuote(const QString& flowNo, const QString& quoteNo, qint64 flowVersion,
                           ApiClient::Handler done)
{
    client_.postJson(kUserBase + QStringLiteral("/flows/%1/quote-confirmations").arg(flowNo),
                     {{"quoteNo", quoteNo}, {"flowVersion", flowVersion}}, std::move(done),
                     idempotencyHeaders());
}
void UserApi::cancelFlow(const QString& flowNo, qint64 flowVersion, const QString& reasonCode,
                         ApiClient::Handler done)
{
    client_.postJson(kUserBase + QStringLiteral("/flows/%1/cancellations").arg(flowNo),
                     {{"reasonCode", reasonCode}, {"flowVersion", flowVersion}}, std::move(done),
                     idempotencyHeaders());
}
void UserApi::startFlow(const QString& flowNo, qint64 flowVersion, ApiClient::Handler done)
{
    client_.postJson(kUserBase + QStringLiteral("/flows/%1/start").arg(flowNo),
                     {{"flowVersion", flowVersion},
                      {"targetAmountCent", QJsonValue::Null},
                      {"balanceFloorCent", QJsonValue::Null}},
                     std::move(done), idempotencyHeaders());
}
void UserApi::progress(const QString& flowNo, ApiClient::Handler done)
{
    client_.get(kUserBase + QStringLiteral("/flows/%1/progress").arg(flowNo), {}, std::move(done));
}
void UserApi::settleFlow(const QString& flowNo, qint64 flowVersion, const QString& reasonCode,
                         ApiClient::Handler done)
{
    client_.postJson(kUserBase + QStringLiteral("/flows/%1/settlements").arg(flowNo),
                     {{"flowVersion", flowVersion}, {"reasonCode", reasonCode}}, std::move(done),
                     idempotencyHeaders());
}

QHash<QByteArray, QByteArray> UserApi::idempotencyHeaders()
{
    return {{"Idempotency-Key", QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8()}};
}

} // namespace ncs::user
