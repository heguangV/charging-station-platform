#pragma once

#include "admin_types.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

#include <initializer_list>

class QNetworkReply;
class QLabel;
class QTableWidget;

namespace ncs::admin
{

QString firstString(const QJsonObject& object, std::initializer_list<const char*> keys);
int firstInt(const QJsonObject& object, std::initializer_list<const char*> keys,
             int fallback = 0);
double firstDouble(const QJsonObject& object, std::initializer_list<const char*> keys,
                   double fallback = 0.0);
QString moneyTextFromObject(const QJsonObject& object, std::initializer_list<const char*> keys);
QString chargerStatusTextFromValue(const QJsonValue& value);
QString userStatusTextFromValue(const QJsonValue& value);
QString normalizeChargerStatus(QString status);
QLabel* heading(const QString& text);
QTableWidget* makeTable(const QStringList& headers);
QJsonObject extractEnvelopeObject(const QByteArray& bodyBytes, QString* errorMessage);
QJsonValue extractPayload(const QByteArray& bodyBytes, QString* errorMessage);
QJsonArray valueToArray(const QJsonValue& value);
QList<QJsonObject> objectsFromValue(const QJsonValue& value);
Station stationFromJson(const QJsonObject& object);
Charger chargerFromJson(const QJsonObject& object);
User userFromJson(const QJsonObject& object);
RevenuePoint revenueFromJson(const QJsonObject& object);
PredictionPoint predictionFromJson(const QJsonObject& object);
QString requestFailureText(QNetworkReply* reply, const QByteArray& bodyBytes);

} // namespace ncs::admin
