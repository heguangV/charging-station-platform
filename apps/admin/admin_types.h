#pragma once

#include <QString>

namespace ncs::admin
{

struct Station
{
    int id = 0;
    QString code;
    QString name;
    QString address;
    double price = 0.0;
    int totalChargers = 0;
    int idleChargers = 0;
    int version = 0;
};

struct Charger
{
    int id = 0;
    int stationId = 0;
    QString code;
    QString stationName;
    QString type;
    double power = 0.0;
    QString status;
    int totalCount = 0;
    int version = 0;
};

struct User
{
    int id = 0;
    QString phone;
    QString nickname;
    QString balance;
    QString status;
    int version = 0;
};

struct RevenuePoint
{
    QString date;
    QString revenue;
    int orders = 0;
};

struct PredictionPoint
{
    QString targetTime;
    QString stationName;
    QString energy;
    int freeCount = 0;
    QString peakFlag;
};

} // namespace ncs::admin
