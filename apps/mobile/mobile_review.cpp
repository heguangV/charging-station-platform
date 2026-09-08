#include "mobile_api.h"
#include <QBuffer>
#ifdef NCS_HAS_POSITIONING
#include <QGeoPositionInfo>
#include <QGeoPositionInfoSource>
#endif
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHttpMultiPart>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QPermissions>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QUrlQuery>
#include <QUuid>
#include <functional>

namespace ncs::mobile
{
void MobileApi::beginReview(const QString& orderNo)
{
    if (!loggedIn() || orderNo.isEmpty())
        return;
    ++reviewRequest_;
    reviewOrderNo_ = orderNo;
    reviewKey_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    reviewInfo_ = QVariantMap{{"orderNo", orderNo}, {"existing", false}, {"submitted", false},
                              {"busy", true},       {"rating", 0},       {"content", QString()},
                              {"error", QString()}};
    emit reviewChanged();
    auto* reply = manager_.get(request(QStringLiteral("/user/orders/%1/review").arg(orderNo)));
    watch(reply);
    reply->setProperty("reviewRequest", reviewRequest_);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply] { handleReviewReply(reply, false, 0, QString()); });
}
void MobileApi::submitReview(int rating, const QString& content)
{
    if (!loggedIn() || reviewOrderNo_.isEmpty() || reviewInfo_.value("busy").toBool())
        return;
    const QString trimmed = content.trimmed();
    if (rating < 1 || rating > 5 || trimmed.isEmpty() || trimmed.toUcs4().size() > 500 ||
        trimmed.contains(QChar(0)))
    {
        setReviewError(QStringLiteral("请选择 1～5 星并填写 1～500 字评价"));
        return;
    }
    reviewInfo_.insert("rating", rating);
    reviewInfo_.insert("content", trimmed);
    reviewInfo_.insert("busy", true);
    emit reviewChanged();
    QNetworkRequest r = request(QStringLiteral("/user/orders/%1/review").arg(reviewOrderNo_));
    r.setRawHeader("Idempotency-Key", reviewKey_.toUtf8());
    const QJsonObject body{{"rating", rating}, {"content", trimmed}};
    auto* reply = manager_.post(r, QJsonDocument(body).toJson());
    reply->setProperty("reviewRequest", reviewRequest_);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, rating, trimmed] { handleReviewReply(reply, true, rating, trimmed); });
}
void MobileApi::handleReviewReply(QNetworkReply* reply, bool submit, int rating,
                                  const QString& content)
{
    if (reply->property("reviewRequest").toInt() != reviewRequest_)
    {
        reply->deleteLater();
        return;
    }
    if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 401)
    {
        reply->deleteLater();
        clearSession();
        setMessage(QStringLiteral("登录已过期，请重新登录"), true);
        return;
    }
    const QByteArray body = reply->readAll();
    const auto json = QJsonDocument::fromJson(body);
    const auto obj = json.object();
    const bool ok = reply->error() == QNetworkReply::NoError && json.isObject() &&
                    obj.value("success").toBool();
    reply->deleteLater();
    if (reply->property("generation").isValid() &&
        reply->property("generation").toInt() != generation_)
        return;
    reviewInfo_.insert("busy", false);
    if (!ok)
    {
        const QString detail = obj.value("userMessage").toString(obj.value("message").toString());
        reviewInfo_.insert("error", detail.isEmpty() ? QStringLiteral("请求失败，请重试") : detail);
        emit reviewChanged();
        return;
    }
    reviewInfo_.insert("error", QString());
    const QJsonObject data = obj.value("data").toObject();
    if (submit)
    {
        const QJsonObject review = data;
        reviewInfo_.insert("existing", true);
        reviewInfo_.insert("submitted", true);
        reviewInfo_.insert("rating", review.value("rating").toInt(rating));
        reviewInfo_.insert("content", review.value("content").toString(content));
        setMessage(QStringLiteral("评价已提交"));
    }
    else
    {
        const QJsonValue review = data.value("review");
        if (review.isObject())
        {
            reviewInfo_.insert("existing", true);
            reviewInfo_.insert("rating", review.toObject().value("rating").toInt());
            reviewInfo_.insert("content", review.toObject().value("content").toString());
        }
    }
    emit reviewChanged();
}
void MobileApi::setReviewError(const QString& message)
{
    reviewInfo_.insert("busy", false);
    reviewInfo_.insert("error", message);
    emit reviewChanged();
}

} // namespace ncs::mobile
