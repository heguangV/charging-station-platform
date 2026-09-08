#pragma once
#include <QString>
namespace ncs::admin
{
struct Station
{
    qint64 id = 0;
    QString code, name, adcode;
    bool enabled = false;
    qint64 version = 0;
};
struct Charger
{
    qint64 id = 0, stationId = 0;
    QString code, type, status;
    qint64 powerWatt = 0, totalCount = 0, totalMinutes = 0, version = 0;
    int statusCode = -1;
};
struct User
{
    qint64 id = 0, balanceCent = 0, registeredAt = 0;
    QString phone, nickname, status;
    int statusCode = -1;
};
struct RevenuePoint
{
    qint64 bucketStart = 0, amountCent = 0;
    int orders = 0;
};
struct PredictionPoint
{
    qint64 stationId = 0, targetAt = 0, energyMwh = 0;
    int idleCount = 0;
    QString peakFlag;
    bool stale = false;
};
} // namespace ncs::admin
