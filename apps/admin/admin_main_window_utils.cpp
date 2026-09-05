#include "admin_main_window_utils.h"

#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QNetworkReply>
#include <QTableWidget>
#include <QWidget>

namespace ncs::admin
{

QString firstString(const QJsonObject& object, std::initializer_list<const char*> keys)
{
    for (const auto* key : keys) {
        const auto value = object.value(QString::fromLatin1(key));
        if (value.isString()) return value.toString();
        if (value.isDouble() || value.isBool()) return value.toVariant().toString();
    }
    return {};
}

int firstInt(const QJsonObject& object, std::initializer_list<const char*> keys, int fallback)
{
    for (const auto* key : keys) {
        const auto value = object.value(QString::fromLatin1(key));
        if (value.isDouble()) return value.toInt(fallback);
        if (value.isString()) {
            bool ok = false;
            const auto parsed = value.toString().toInt(&ok);
            if (ok) return parsed;
        }
    }
    return fallback;
}

double firstDouble(const QJsonObject& object, std::initializer_list<const char*> keys,
                   double fallback)
{
    for (const auto* key : keys) {
        const auto value = object.value(QString::fromLatin1(key));
        if (value.isDouble()) return value.toDouble(fallback);
        if (value.isString()) return value.toString().toDouble();
    }
    return fallback;
}

QString moneyTextFromObject(const QJsonObject& object, std::initializer_list<const char*> keys)
{
    for (const auto* key : keys) {
        const auto name = QString::fromLatin1(key);
        if (!object.contains(name)) continue;
        const auto value = object.value(name);
        if (name.contains(QStringLiteral("Cent"), Qt::CaseInsensitive)) {
            return QStringLiteral("%1").arg(value.toVariant().toLongLong() / 100.0, 0, 'f', 2);
        }
        if (value.isDouble()) return QStringLiteral("%1").arg(value.toDouble(), 0, 'f', 2);
        if (value.isString()) return value.toString();
    }
    return QStringLiteral("0.00");
}

QString chargerStatusTextFromValue(const QJsonValue& value)
{
    if (value.isString()) return value.toString();
    if (!value.isDouble()) return {};
    switch (value.toInt()) {
    case 0: return QStringLiteral("空闲");
    case 1: return QStringLiteral("使用中");
    case 2: return QStringLiteral("故障");
    case 3: return QStringLiteral("已停用");
    case 4: return QStringLiteral("重启中");
    default: return QString::number(value.toInt());
    }
}

QString userStatusTextFromValue(const QJsonValue& value)
{
    if (value.isString()) return value.toString();
    if (!value.isDouble()) return {};
    switch (value.toInt()) {
    case 0: return QStringLiteral("冻结");
    case 1: return QStringLiteral("正常");
    default: return QString::number(value.toInt());
    }
}

QString normalizeChargerStatus(QString status)
{
    if (status == QStringLiteral("闲置")) return QStringLiteral("空闲");
    if (status == QStringLiteral("在用")) return QStringLiteral("使用中");
    if (status == QStringLiteral("停用")) return QStringLiteral("已停用");
    if (status == QStringLiteral("重启")) return QStringLiteral("重启中");
    return status;
}

QLabel* heading(const QString& text)
{
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("pageTitle"));
    return label;
}

QTableWidget* makeTable(const QStringList& headers)
{
    auto* table = new QTableWidget;
    table->setColumnCount(headers.size());
    table->setHorizontalHeaderLabels(headers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setAlternatingRowColors(true);
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->setVisible(false);
    return table;
}

QJsonObject extractEnvelopeObject(const QByteArray& bodyBytes, QString* errorMessage)
{
    const auto document = QJsonDocument::fromJson(bodyBytes);
    if (!document.isObject()) {
        if (errorMessage) *errorMessage = QStringLiteral("响应格式错误");
        return {};
    }
    const auto object = document.object();
    const auto code = object.value(QStringLiteral("code")).toInt(0);
    const auto success = object.value(QStringLiteral("success")).toBool(code == 0);
    if (!success || code != 0) {
        if (errorMessage) {
            *errorMessage =
                object.value(QStringLiteral("userMessage")).toString(QStringLiteral("请求失败"));
        }
        return {};
    }
    return object;
}

QJsonValue extractPayload(const QByteArray& bodyBytes, QString* errorMessage)
{
    const auto envelope = extractEnvelopeObject(bodyBytes, errorMessage);
    if (envelope.isEmpty()) return {};
    return envelope.value(QStringLiteral("data"));
}

QJsonArray valueToArray(const QJsonValue& value)
{
    if (value.isArray()) return value.toArray();
    if (value.isObject()) {
        const auto object = value.toObject();
        for (const auto* key : {"items", "list", "records", "rows", "predictions", "revenue30d"}) {
            const auto candidate = object.value(QString::fromLatin1(key));
            if (candidate.isArray()) return candidate.toArray();
        }
    }
    return {};
}

QList<QJsonObject> objectsFromValue(const QJsonValue& value)
{
    QList<QJsonObject> result;
    const auto array = valueToArray(value);
    for (const auto& item : array) {
        if (item.isObject()) result.append(item.toObject());
    }
    return result;
}

Station stationFromJson(const QJsonObject& object)
{
    Station station;
    station.id = firstInt(object, {"id", "stationId"});
    station.code = firstString(object, {"code"});
    station.name = firstString(object, {"name", "stationName"});
    station.address = firstString(object, {"address", "fullAddress"});
    if (object.contains(QStringLiteral("price"))) {
        station.price = firstDouble(object, {"price"});
    } else if (object.contains(QStringLiteral("priceCentPerKwh"))) {
        station.price = firstDouble(object, {"priceCentPerKwh"}) / 100.0;
    } else if (object.contains(QStringLiteral("electricityPriceCentPerKwh")) ||
               object.contains(QStringLiteral("servicePriceCentPerKwh"))) {
        station.price = (firstDouble(object, {"electricityPriceCentPerKwh"}) +
                         firstDouble(object, {"servicePriceCentPerKwh"})) /
                        100.0;
    }
    station.totalChargers = firstInt(object, {"totalChargers", "chargerCount", "count"});
    station.idleChargers = firstInt(object, {"idleChargers", "freeCount", "availableChargers"});
    station.version = firstInt(object, {"version"});
    return station;
}

Charger chargerFromJson(const QJsonObject& object)
{
    Charger charger;
    charger.id = firstInt(object, {"id", "chargerId"});
    charger.stationId = firstInt(object, {"stationId"});
    charger.code = firstString(object, {"code", "chargerCode"});
    charger.stationName = firstString(object, {"stationName", "name"});
    charger.type = firstString(object, {"type", "chargerTypeText"});
    charger.power = object.contains(QStringLiteral("power")) ? firstDouble(object, {"power"})
                                                             : firstDouble(object, {"powerWatt"}) / 1000.0;
    charger.status = firstString(object, {"statusText", "status"});
    if (charger.status.isEmpty()) charger.status = chargerStatusTextFromValue(object.value(QStringLiteral("status")));
    charger.status = normalizeChargerStatus(charger.status);
    charger.totalCount = firstInt(object, {"totalCount", "chargeCount", "sessionCount"});
    charger.version = firstInt(object, {"version"});
    return charger;
}

User userFromJson(const QJsonObject& object)
{
    User user;
    user.id = firstInt(object, {"id", "userId"});
    user.phone = firstString(object, {"phone", "maskedPhone", "phoneMasked"});
    user.nickname = firstString(object, {"nickname", "name"});
    user.balance = object.contains(QStringLiteral("balance"))
                       ? firstString(object, {"balance"})
                       : moneyTextFromObject(object, {"balanceCent"});
    user.status = firstString(object, {"statusText", "status"});
    if (user.status.isEmpty()) user.status = userStatusTextFromValue(object.value(QStringLiteral("status")));
    user.version = firstInt(object, {"version"});
    return user;
}

RevenuePoint revenueFromJson(const QJsonObject& object)
{
    RevenuePoint point;
    point.date = firstString(object, {"date", "bucketAt", "day", "time"});
    point.revenue = moneyTextFromObject(object, {"revenue", "revenueCent", "totalRevenueCent"});
    point.orders = firstInt(object, {"orders", "orderCount"});
    return point;
}

PredictionPoint predictionFromJson(const QJsonObject& object)
{
    PredictionPoint point;
    point.targetTime = firstString(object, {"targetTime", "targetAt", "time"});
    point.stationName = firstString(object, {"stationName", "name"});
    if (object.contains(QStringLiteral("predictedEnergyMwh"))) {
        point.energy = QStringLiteral("%1 kWh").arg(
            object.value(QStringLiteral("predictedEnergyMwh")).toVariant().toLongLong() / 1000000.0,
            0, 'f', 2);
    } else if (object.contains(QStringLiteral("energyMwh"))) {
        point.energy = QStringLiteral("%1 kWh").arg(
            object.value(QStringLiteral("energyMwh")).toVariant().toLongLong() / 1000000.0, 0, 'f',
            2);
    } else {
        point.energy = firstString(object, {"predictedEnergy", "energy"});
    }
    point.freeCount = firstInt(object, {"predictedFreeCount", "freeCount"});
    point.peakFlag = object.value(QStringLiteral("isPeak")).toBool() ? QStringLiteral("高峰")
                                                                      : QStringLiteral("平峰");
    if (point.peakFlag.isEmpty()) point.peakFlag = firstString(object, {"peakFlag"});
    return point;
}

QString requestFailureText(QNetworkReply* reply, const QByteArray& bodyBytes)
{
    QString envelopeError;
    const auto envelope = extractEnvelopeObject(bodyBytes, &envelopeError);
    if (!envelope.isEmpty()) return {};
    if (!bodyBytes.isEmpty() && !envelopeError.isEmpty() &&
        reply->error() != QNetworkReply::ConnectionRefusedError &&
        reply->error() != QNetworkReply::HostNotFoundError &&
        reply->error() != QNetworkReply::TimeoutError &&
        reply->error() != QNetworkReply::SslHandshakeFailedError) {
        return envelopeError;
    }
    switch (reply->error()) {
    case QNetworkReply::ConnectionRefusedError:
        return QStringLiteral("连接被拒绝，请检查后端服务是否已启动");
    case QNetworkReply::HostNotFoundError:
        return QStringLiteral("找不到服务器地址，请检查后端配置");
    case QNetworkReply::TimeoutError:
        return QStringLiteral("请求超时，请稍后重试");
    case QNetworkReply::SslHandshakeFailedError:
        return QStringLiteral("SSL 握手失败，请检查证书配置");
    default:
        return reply->errorString();
    }
}

} // namespace ncs::admin
