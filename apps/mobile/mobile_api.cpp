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
MobileApi::MobileApi(QObject* parent) : QObject(parent)
{
    baseUrl_ = qEnvironmentVariable(
        "NCS_API_BASE_URL",
        QSettings().value("serverUrl", "http://127.0.0.1:18443/api/v1").toString());
    manager_.setProxy(QNetworkProxy::NoProxy);
    connect(&manager_, &QNetworkAccessManager::finished, this,
            [this](QNetworkReply* reply)
            {
                if (reply->property("ncsTracked").toBool())
                {
                    --pending_;
                    emit busyChanged();
                }
            });
}
void MobileApi::setMessage(const QString& value, bool error)
{
    if (message_ == value && messageError_ == error)
        return;
    message_ = value;
    messageError_ = error;
    emit messageChanged();
}
QNetworkRequest MobileApi::request(const QString& path, const QByteArray& contentType) const
{
    QNetworkRequest r(QUrl(baseUrl_ + path));
    if (!contentType.isEmpty())
        r.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    if (!token_.isEmpty())
        r.setRawHeader("Authorization", "Bearer " + token_.toUtf8());
    // adb reverse 隧道下复用 keep-alive 连接，首次大体积请求后该流可能被对端关闭，
    // 复用会直接连接层报错（Qt 不自动重试 POST）。逐请求新建连接最稳。
    r.setRawHeader("Connection", "close");
    r.setTransferTimeout(15000);
    r.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                   QNetworkRequest::SameOriginRedirectPolicy);
    return r;
}
void MobileApi::parseReply(QNetworkReply* reply, std::function<void(const QJsonObject&)> done,
                           const std::function<void(const QString&)>& fail)
{
    if (reply->property("generation").isValid() &&
        reply->property("generation").toInt() != generation_)
    {
        reply->deleteLater();
        return;
    }
    const auto json = QJsonDocument::fromJson(reply->readAll());
    const auto obj = json.object();
    if (reply->error() != QNetworkReply::NoError || !json.isObject() ||
        !obj.value("success").toBool())
    {
        QString message = obj.value("userMessage").toString();
        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 401)
        {
            clearSession();
            message = QStringLiteral("登录已过期，请重新登录");
        }
        if (message.isEmpty())
            message =
                QStringLiteral("无法连接服务端，请检查网络和服务器地址；USB 联调请重新连接设备");
        setMessage(message, true);
        if (fail)
            fail(message);
        reply->deleteLater();
        return;
    }
    const auto key = reply->property("mutationScope").toString();
    if (!key.isEmpty())
        mutationKeys_.remove(key);
    done(obj.value("data").toObject());
    reply->deleteLater();
}
void MobileApi::watch(QNetworkReply* reply)
{
    reply->setProperty("generation", generation_);
    reply->setProperty("ncsTracked", true);
    ++pending_;
    emit busyChanged();
}
void MobileApi::get(const QString& path, std::function<void(const QJsonObject&)> done,
                    std::function<void(const QString&)> fail)
{
    auto* r = manager_.get(request(path));
    watch(r);
    connect(r, &QNetworkReply::finished, this,
            [this, r, done = std::move(done), fail = std::move(fail)]() mutable
            { parseReply(r, std::move(done), std::move(fail)); });
}
void MobileApi::postJson(const QString& path, const QJsonObject& body,
                         std::function<void(const QJsonObject&)> done)
{
    if (busy())
        return;
    auto r = request(path);
    const auto payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    const QString scope = path + QString::fromUtf8(payload);
    if (!mutationKeys_.contains(scope))
        mutationKeys_.insert(scope, QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8());
    r.setRawHeader("Idempotency-Key", mutationKeys_.value(scope));
    auto* reply = manager_.post(r, payload);
    watch(reply);
    reply->setProperty("mutationScope", scope);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, done = std::move(done)]() mutable
            { parseReply(reply, std::move(done)); });
}
void MobileApi::requestCode(const QString& phone)
{
    if (busy())
        return;
    if (!QRegularExpression("^1[0-9]{10}$").match(phone).hasMatch())
    {
        setMessage(QStringLiteral("请输入正确的 11 位手机号"), true);
        return;
    }
    auto r = request("/user/auth/sms/code");
    const QJsonObject body{{"phone", phone}, {"purpose", "LOGIN"}};
    auto* reply = manager_.post(r, QJsonDocument(body).toJson());
    watch(reply);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply]
            {
                parseReply(reply,
                           [this](const QJsonObject& d)
                           {
                               emit codeSent(60);
                               setMessage(d.value("developmentCode").toString().isEmpty()
                                              ? QStringLiteral("验证码已发送")
                                              : QStringLiteral("开发验证码：%1")
                                                    .arg(d.value("developmentCode").toString()));
                           });
            });
}
void MobileApi::login(const QString& phone, const QString& code)
{
    if (busy())
        return;
    if (!QRegularExpression("^1[0-9]{10}$").match(phone).hasMatch() ||
        !QRegularExpression("^[0-9]{6}$").match(code).hasMatch())
    {
        setMessage(QStringLiteral("请输入正确的手机号和 6 位验证码"), true);
        return;
    }
    loginPhone_ = phone;
    auto r = request("/user/auth/login/sms");
    const QJsonObject body{{"phone", phone}, {"smsCode", code}, {"deviceId", "android-mobile"}};
    auto* reply = manager_.post(r, QJsonDocument(body).toJson());
    watch(reply);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply]
            {
                parseReply(reply,
                           [this](const QJsonObject& d)
                           {
                               token_ = d.value("accessToken").toString();
                               setMessage(QStringLiteral("登录成功"));
                               emit sessionChanged();
                               loadProfile();
                               loadStations();
                           });
            });
}
void MobileApi::loadStations(const QString& keyword)
{
    if (!loggedIn())
        return;
    const int serial = ++stationRequest_;
    QUrlQuery q;
    q.addQueryItem("latitudeE6", QString::number(latitudeE6_));
    q.addQueryItem("longitudeE6", QString::number(longitudeE6_));
    q.addQueryItem("pageSize", "100");
    if (!keyword.isEmpty())
        q.addQueryItem("keyword", keyword);
    get(QStringLiteral("/user/stations?%1").arg(q.toString(QUrl::FullyEncoded)),
        [this, serial, keyword](const QJsonObject& d)
        {
            if (serial != stationRequest_)
                return;
            stations_.clear();
            for (const auto& v : d.value("items").toArray())
            {
                const auto item = v.toObject();
                if (keyword.trimmed().isEmpty() ||
                    item.value("name").toString().contains(keyword.trimmed(),
                                                           Qt::CaseInsensitive) ||
                    item.value("address").toString().contains(keyword.trimmed(),
                                                              Qt::CaseInsensitive))
                    stations_.append(item.toVariantMap());
            }
            emit stationsChanged();
        });
}
void MobileApi::loadStationChargers(qint64 stationId)
{
    if (!loggedIn() || stationId <= 0)
        return;
    const int serial = ++chargerRequest_;
    chargers_.clear();
    emit chargersChanged();
    get(QStringLiteral("/user/stations/%1/chargers?page=1&pageSize=50").arg(stationId),
        [this, serial](const QJsonObject& d)
        {
            if (serial != chargerRequest_)
                return;
            for (const auto& value : d.value("items").toArray())
                chargers_.append(value.toObject().toVariantMap());
            emit chargersChanged();
        });
}
void MobileApi::loadStationReviews(qint64 stationId)
{
    if (!loggedIn() || stationId <= 0)
        return;
    // UC-U-12 场站评论墙：加载/空/失败三态，过期回复按请求序号丢弃。
    const int serial = ++wallRequest_;
    stationReviews_.clear();
    stationReviewsError_.clear();
    stationReviewsBusy_ = true;
    emit stationReviewsChanged();
    get(QStringLiteral("/user/stations/%1/reviews").arg(stationId),
        [this, serial](const QJsonObject& d)
        {
            if (serial != wallRequest_)
                return;
            stationReviewsBusy_ = false;
            for (const auto& v : d.value("items").toArray())
            {
                auto item = v.toObject().toVariantMap();
                item.insert(QStringLiteral("timeLabel"),
                            QDateTime::fromSecsSinceEpoch(item.value("createdAt").toLongLong())
                                .date()
                                .toString(QStringLiteral("MM-dd")));
                stationReviews_.append(item);
            }
            emit stationReviewsChanged();
        },
        [this, serial](const QString& message)
        {
            if (serial != wallRequest_)
                return;
            stationReviewsBusy_ = false;
            stationReviewsError_ = QStringLiteral("评论加载失败：") + message;
            emit stationReviewsChanged();
        });
}
void MobileApi::loadOrders()
{
    if (!loggedIn())
        return;
    get("/user/orders?page=1&pageSize=100&sort=-createdAt",
        [this](const QJsonObject& d)
        {
            orders_.clear();
            for (const auto& v : d.value("items").toArray())
                orders_.append(v.toObject().toVariantMap());
            emit ordersChanged();
        });
}
void MobileApi::loadActiveFlow()
{
    if (!loggedIn())
        return;
    get("/user/flows/active",
        [this](const QJsonObject& d)
        {
            const QJsonObject active = d.value("flow").toObject();
            flow_ = d.value("hasActiveFlow").toBool() && !active.isEmpty() ? active.toVariantMap()
                                                                           : QVariantMap{};
            emit flowChanged();
        });
}
void MobileApi::requestCharge(qint64 stationId, int chargerType, qint64 preferredChargerId)
{
    if (!flow_.isEmpty())
    {
        setMessage(QStringLiteral("请先处理当前未结算的充电订单"), true);
        emit flowChanged();
        return;
    }
    QJsonObject body{{"stationId", stationId}, {"chargerType", chargerType}};
    body.insert("preferredChargerId", preferredChargerId > 0 ? QJsonValue(preferredChargerId)
                                                             : QJsonValue(QJsonValue::Null));
    postJson("/user/flows", body,
             [this](const QJsonObject& d)
             {
                 flow_ = d.toVariantMap();
                 emit flowChanged();
                 setMessage(QStringLiteral("已提交充电请求，请确认报价"));
             });
}
void MobileApi::confirmCharge()
{
    if (flow_.isEmpty())
        return;
    const auto quote = flow_.value("quote").toMap();
    postJson(
        QStringLiteral("/user/flows/%1/quote-confirmations").arg(flow_.value("flowNo").toString()),
        {{"quoteNo", quote.value("quoteNo").toString()},
         {"flowVersion", flow_.value("version").toLongLong()}},
        [this](const QJsonObject& d)
        {
            flow_ = d.toVariantMap();
            emit flowChanged();
            setMessage(QStringLiteral("报价已确认"));
        });
}
void MobileApi::startCharge()
{
    if (flow_.isEmpty())
        return;
    postJson(QStringLiteral("/user/flows/%1/start").arg(flow_.value("flowNo").toString()),
             {{"flowVersion", flow_.value("version").toLongLong()}},
             [this](const QJsonObject& d)
             {
                 flow_ = d.toVariantMap();
                 emit flowChanged();
                 setMessage(QStringLiteral("充电已开始"));
             });
}
void MobileApi::settleCharge()
{
    if (flow_.isEmpty())
        return;
    postJson(
        QStringLiteral("/user/flows/%1/settlements").arg(flow_.value("flowNo").toString()),
        {{"flowVersion", flow_.value("version").toLongLong()}, {"reasonCode", "USER_FINISHED"}},
        [this](const QJsonObject& d)
        {
            receipt_ = d.toVariantMap();
            emit receiptChanged();
            flow_.clear();
            emit flowChanged();
            loadOrders();
            loadProfile();
            setMessage(QStringLiteral("充电已结算"));
        });
}
void MobileApi::cancelCharge()
{
    if (flow_.isEmpty())
        return;
    postJson(
        QStringLiteral("/user/flows/%1/cancellations").arg(flow_.value("flowNo").toString()),
        {{"flowVersion", flow_.value("version").toLongLong()}, {"reasonCode", "USER_CANCELLED"}},
        [this](const QJsonObject&)
        {
            flow_.clear();
            emit flowChanged();
            setMessage(QStringLiteral("预约已取消"));
        });
}
void MobileApi::loadFlowProgress()
{
    const QString number = flow_.value("flowNo").toString();
    if (!loggedIn() || number.isEmpty() || busy())
        return;
    get(QStringLiteral("/user/flows/%1").arg(number),
        [this, number](const QJsonObject& d)
        {
            if (flow_.value("flowNo").toString() != number)
                return;
            for (auto it = d.constBegin(); it != d.constEnd(); ++it)
                flow_.insert(it.key(), it.value().toVariant());
            emit flowChanged();
            const int status = d.value("status").toInt();
            if (status >= 60)
            {
                loadActiveFlow();
                loadOrders();
                loadProfile();
                return;
            }
            if (status != 40 && status != 50)
                return;
            get(QStringLiteral("/user/flows/%1/progress").arg(number),
                [this, number](const QJsonObject& progress)
                {
                    if (flow_.value("flowNo").toString() != number)
                        return;
                    for (auto it = progress.constBegin(); it != progress.constEnd(); ++it)
                        flow_.insert(it.key(), it.value().toVariant());
                    emit flowChanged();
                });
        });
}
bool MobileApi::configureServer(const QString& value)
{
    if (loggedIn() || busy())
    {
        setMessage(QStringLiteral("请退出登录后修改服务器地址"), true);
        return false;
    }
    QUrl url(value.trimmed());
    const bool loopback =
        url.host() == "127.0.0.1" || url.host() == "localhost" || url.host() == "::1";
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasQuery() ||
        url.hasFragment() || (url.scheme() != "https" && !(url.scheme() == "http" && loopback)))
    {
        setMessage(QStringLiteral("请输入 HTTPS 地址；USB 联调可使用 http://127.0.0.1:18443"),
                   true);
        return false;
    }
    QString path = url.path();
    while (path.endsWith('/'))
        path.chop(1);
    if (path.isEmpty())
        path = "/api/v1";
    if (path != "/api/v1")
    {
        setMessage(QStringLiteral("服务地址路径应为 /api/v1"), true);
        return false;
    }
    url.setPath(path);
    baseUrl_ = url.toString();
    QSettings().setValue("serverUrl", baseUrl_);
    manager_.clearConnectionCache();
    emit serverChanged();
    setMessage(QStringLiteral("服务器地址已保存"));
    return true;
}
void MobileApi::loadOrder(const QString& orderNo)
{
    get(QStringLiteral("/user/orders/%1").arg(orderNo),
        [this](const QJsonObject& d)
        {
            receipt_ = d.toVariantMap();
            emit receiptChanged();
        });
}

} // namespace ncs::mobile
