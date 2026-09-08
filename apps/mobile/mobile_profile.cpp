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
void MobileApi::loadProfile()
{
    if (!loggedIn())
        return;
    get("/user/me",
        [this](const QJsonObject& d)
        {
            profileVersion_ = d.value("version").toInteger();
            createdAt_ = QDateTime::fromSecsSinceEpoch(d.value("registeredAt").toInteger())
                             .toLocalTime()
                             .toString("yyyy-MM-dd");
            nickname_ = d.value("nickname").toString();
            phone_ = d.value("phoneMasked").toString();
            balanceCent_ = d.value("balanceCent").toInteger();
            emit profileChanged();
            if (d.value("avatarUrl").isNull() || d.value("avatarUrl").toString().isEmpty())
            {
                if (!avatarData_.isEmpty())
                {
                    avatarData_.clear();
                    emit avatarChanged();
                }
                return;
            }
            auto* r = manager_.get(request("/user/me/avatar/content"));
            r->setProperty("generation", generation_);
            connect(r, &QNetworkReply::finished, this,
                    [this, r]
                    {
                        if (r->property("generation").toInt() != generation_)
                        {
                            r->deleteLater();
                            return;
                        }
                        const QByteArray data = r->readAll();
                        if (r->error() == QNetworkReply::NoError && !data.isEmpty())
                        {
                            avatarData_ = QString::fromLatin1(data.toBase64());
                            emit avatarChanged();
                        }
                        else if (!avatarData_.isEmpty())
                        {
                            avatarData_.clear();
                            emit avatarChanged();
                        }
                        r->deleteLater();
                    });
        });
}
void MobileApi::uploadAvatar(const QString& path)
{
    if (!loggedIn() || busy())
        return;
    const QUrl inputUrl(path);
    const QString localPath = inputUrl.isLocalFile() ? inputUrl.toLocalFile() : path;
    QFile source(localPath);
    if (!source.open(QIODevice::ReadOnly) || source.size() > 5 * 1024 * 1024)
    {
        setMessage(QStringLiteral("请选择不超过 5 MiB 的图片"), true);
        discardCapture();
        return;
    }
    QImageReader reader(&source);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (!size.isValid() || size.width() > 16000 || size.height() > 16000)
    {
        setMessage(QStringLiteral("图片尺寸无效或过大"), true);
        discardCapture();
        return;
    }
    reader.setScaledSize(size.scaled(1024, 1024, Qt::KeepAspectRatio));
    QImage image = reader.read();
    source.close();
    discardCapture();
    if (image.isNull())
    {
        setMessage(QStringLiteral("无法读取照片，请重新拍摄"), true);
        return;
    }
    const int edge = qMin(image.width(), image.height());
    image = image.copy((image.width() - edge) / 2, (image.height() - edge) / 2, edge, edge)
                .scaled(512, 512, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QByteArray encoded;
    QBuffer buffer(&encoded);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "JPEG", 80))
    {
        setMessage(QStringLiteral("照片编码失败"), true);
        return;
    }
    if (encoded.size() > 200 * 1024)
    {
        setMessage(QStringLiteral("图片压缩后仍过大，请重新选择"), true);
        return;
    }
    if (QFileInfo(localPath).absoluteFilePath() ==
        QFileInfo(avatarCapturePath()).absoluteFilePath())
        QFile::remove(localPath);
    auto* multi = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    QHttpPart part;
    part.setHeader(QNetworkRequest::ContentDispositionHeader,
                   QStringLiteral("form-data; name=\"file\"; filename=\"camera.jpg\""));
    part.setHeader(QNetworkRequest::ContentTypeHeader, "image/jpeg");
    part.setBody(encoded);
    multi->append(part);
    auto* reply = manager_.post(request("/user/me/avatar", QByteArray()), multi);
    multi->setParent(reply);
    watch(reply);
    avatarUploading_ = true;
    emit busyChanged();
    connect(reply, &QNetworkReply::finished, this,
            [this, reply]
            {
                avatarUploading_ = false;
                emit busyChanged();
                parseReply(reply,
                           [this](const QJsonObject&)
                           {
                               setMessage(QStringLiteral("头像已更新"));
                               auto* avatar = manager_.get(request("/user/me/avatar/content"));
                               avatar->setProperty("generation", generation_);
                               connect(avatar, &QNetworkReply::finished, this,
                                       [this, avatar]
                                       {
                                           if (avatar->property("generation").toInt() !=
                                               generation_)
                                           {
                                               avatar->deleteLater();
                                               return;
                                           }
                                           const QByteArray data = avatar->readAll();
                                           if (avatar->error() == QNetworkReply::NoError &&
                                               !data.isEmpty())
                                           {
                                               avatarData_ = QString::fromLatin1(data.toBase64());
                                               emit avatarChanged();
                                           }
                                           avatar->deleteLater();
                                       });
                           });
            });
}
void MobileApi::logout()
{
    if (!loggedIn() || busy())
        return;
    auto* r = manager_.post(request("/user/auth/logout"), QByteArray("{}"));
    watch(r);
    connect(r, &QNetworkReply::finished, this,
            [this, r]
            {
                r->deleteLater();
                clearSession();
                setMessage(QStringLiteral("已退出登录"));
            });
}

// UC-U-12：订单评价。beginReview 生成固定 Idempotency-Key，失败重试沿用，保证幂等。
void MobileApi::clearSession()
{
    ++reviewRequest_;
    ++stationRequest_;
    ++chargerRequest_;
    ++routeRequest_;
    ++locationRequest_;
    locating_ = false;
    emit locationChanged();
    ++generation_;
    mutationKeys_.clear();
    nickname_.clear();
    phone_.clear();
    loginPhone_.clear();
    createdAt_.clear();
    balanceCent_ = 0;
    emit profileChanged();
    token_.clear();
    orders_.clear();
    stations_.clear();
    chargers_.clear();
    flow_.clear();
    receipt_.clear();
    route_.clear();
    reviewInfo_.clear();
    reviewOrderNo_.clear();
    avatarData_.clear();
    emit ordersChanged();
    emit stationsChanged();
    emit chargersChanged();
    emit flowChanged();
    emit receiptChanged();
    emit routeChanged();
    emit avatarChanged();
    emit reviewChanged();
    emit sessionChanged();
}
void MobileApi::updateNickname(const QString& nickname)
{
    if (!loggedIn() || busy())
        return;
    const QString name = nickname.trimmed();
    if (name.isEmpty() || name.toUcs4().size() > 20)
    {
        setMessage(QStringLiteral("昵称须为 1～20 字"), true);
        return;
    }
    auto* r = manager_.put(
        request("/user/me"),
        QJsonDocument(QJsonObject{{"nickname", name}, {"version", profileVersion_}}).toJson());
    watch(r);
    connect(r, &QNetworkReply::finished, this,
            [this, r]
            {
                parseReply(r,
                           [this](const QJsonObject&)
                           {
                               loadProfile();
                               setMessage(QStringLiteral("昵称已保存"));
                           });
            });
}
void MobileApi::recharge(const QString& amount)
{
    if (!loggedIn())
        return;
    const QString value = amount.trimmed();
    if (!QRegularExpression("^[0-9]{1,5}(\\.[0-9]{1,2})?$").match(value).hasMatch())
    {
        setMessage(QStringLiteral("请输入 0.01～10000 元，最多两位小数"), true);
        return;
    }
    const auto parts = value.split('.');
    const qint64 cents = parts[0].toLongLong() * 100 +
                         (parts.size() == 2 ? parts[1].leftJustified(2, '0').toInt() : 0);
    if (cents < 1 || cents > 1000000)
    {
        setMessage(QStringLiteral("请输入 0.01～10000 元"), true);
        return;
    }
    postJson("/user/wallet/recharges", {{"amountCent", cents}},
             [this](const QJsonObject&)
             {
                 loadProfile();
                 setMessage(QStringLiteral("余额充值成功"));
             });
}
void MobileApi::requestDeletionCode()
{
    if (!loggedIn())
        return;
    postJson(
        "/user/auth/sms/code", {{"phone", loginPhone_}, {"purpose", "RESET_PASSWORD"}},
        [this](const QJsonObject& d)
        {
            setMessage(
                d.value("developmentCode").toString().isEmpty()
                    ? QStringLiteral("注销验证码已发送")
                    : QStringLiteral("开发验证码：%1").arg(d.value("developmentCode").toString()));
        });
}
void MobileApi::deleteAccount(const QString& code)
{
    if (!loggedIn() || busy())
        return;
    if (!QRegularExpression("^[0-9]{6}$").match(code).hasMatch())
    {
        setMessage(QStringLiteral("请输入 6 位验证码"), true);
        return;
    }
    auto* r = manager_.sendCustomRequest(
        request("/user/me"), "DELETE",
        QJsonDocument(
            QJsonObject{{"confirm", true}, {"password", QJsonValue::Null}, {"smsCode", code}})
            .toJson());
    watch(r);
    connect(r, &QNetworkReply::finished, this,
            [this, r]
            {
                parseReply(r,
                           [this](const QJsonObject&)
                           {
                               clearSession();
                               setMessage(QStringLiteral("账户已注销"));
                           });
            });
}

} // namespace ncs::mobile
