#pragma once
#include "admin_types.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QTimeZone>
#include <QUrlQuery>
class QLabel;
class QTableWidget;
namespace ncs::admin
{
QString money(qint64 cent);
QString dateTimeText(qint64 timestamp, const QString& format = QStringLiteral("yyyy-MM-dd HH:mm"));
QString chargerStatusText(int status);
QString peakText(const QString& flag);
QTimeZone businessTimeZone();
QUrlQuery revenueQuery(qint64 from, qint64 to);
QLabel* heading(const QString& text);
QTableWidget* makeTable(const QStringList& headers);
Station stationFromJson(const QJsonObject& object);
Charger chargerFromJson(const QJsonObject& object);
User userFromJson(const QJsonObject& object);
RevenuePoint revenueFromJson(const QJsonObject& object);
PredictionPoint predictionFromJson(const QJsonObject& object);
QList<RevenuePoint> groupRevenueByDay(const QJsonArray& items);
} // namespace ncs::admin
