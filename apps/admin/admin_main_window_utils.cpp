#include "admin_main_window_utils.h"
#include <QDateTime>
#include <QHeaderView>
#include <QLabel>
#include <QMap>
#include <QTableWidget>
namespace ncs::admin
{
QString money(qint64 cent)
{
    return QStringLiteral("¥%1").arg(cent / 100.0, 0, 'f', 2);
}
QTimeZone businessTimeZone()
{
    return QTimeZone("Asia/Shanghai");
}
QString dateTimeText(qint64 at, const QString& format)
{
    return at > 0 ? QDateTime::fromSecsSinceEpoch(at, businessTimeZone()).toString(format)
                  : QStringLiteral("—");
}
QString chargerStatusText(int status)
{
    const QStringList values{QStringLiteral("空闲"), QStringLiteral("使用中"),
                             QStringLiteral("故障"), QStringLiteral("已停用"),
                             QStringLiteral("重启中")};
    return status >= 0 && status < values.size() ? values[status] : QStringLiteral("未知");
}
QString peakText(const QString& flag)
{
    if (flag == "PEAK")
        return QStringLiteral("高峰");
    if (flag == "VALLEY")
        return QStringLiteral("低谷");
    if (flag == "NORMAL" || flag == "FLAT")
        return QStringLiteral("平峰");
    return flag.isEmpty() ? QStringLiteral("—") : flag;
}
QUrlQuery revenueQuery(qint64 from, qint64 to)
{
    QUrlQuery q;
    q.addQueryItem("fromAt", QString::number(from));
    q.addQueryItem("toAt", QString::number(to));
    q.addQueryItem("bucket", "hour");
    return q;
}
QLabel* heading(const QString& text)
{
    auto* label = new QLabel(text);
    label->setTextFormat(Qt::PlainText);
    label->setObjectName("pageTitle");
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
    table->setShowGrid(false);
    table->setAlternatingRowColors(true);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->horizontalHeader()->setMinimumSectionSize(80);
    table->verticalHeader()->setVisible(false);
    table->verticalHeader()->setDefaultSectionSize(48);
    table->setWordWrap(false);
    return table;
}
Station stationFromJson(const QJsonObject& o)
{
    return {o.value("id").toInteger(),   o.value("code").toString(),
            o.value("name").toString(),  o.value("adcode").toString(),
            o.value("enabled").toBool(), o.value("version").toInteger()};
}
Charger chargerFromJson(const QJsonObject& o)
{
    Charger c;
    c.id = o.value("id").toInteger();
    c.stationId = o.value("stationId").toInteger();
    c.code = o.value("code").toString();
    const int type = o.value("chargerType").toInt(-1);
    c.type = type == 1   ? QStringLiteral("直流快充")
             : type == 0 ? QStringLiteral("交流慢充")
                         : QStringLiteral("未知");
    c.statusCode = o.value("status").toInt(-1);
    c.status = chargerStatusText(c.statusCode);
    c.powerWatt = o.value("powerWatt").toInteger();
    c.totalCount = o.value("totalCount").toInteger();
    c.totalMinutes = o.value("totalMinutes").toInteger();
    c.version = o.value("version").toInteger();
    return c;
}
User userFromJson(const QJsonObject& o)
{
    User u;
    u.id = o.value("id").toInteger();
    u.phone = o.value("phoneMasked").toString();
    u.nickname = o.value("nickname").toString();
    u.balanceCent = o.value("balanceCent").toInteger();
    u.registeredAt = o.value("registeredAt").toInteger();
    u.statusCode = o.value("status").toInt(-1);
    u.status = u.statusCode == 0   ? QStringLiteral("冻结")
               : u.statusCode == 1 ? QStringLiteral("正常")
                                   : QStringLiteral("未知");
    return u;
}
RevenuePoint revenueFromJson(const QJsonObject& o)
{
    return {o.value("bucketStart").toInteger(), o.value("amountCent").toInteger(),
            o.value("orderCount").toInt()};
}
PredictionPoint predictionFromJson(const QJsonObject& o)
{
    return {
        o.value("stationId").toInteger(),
        o.value("targetAt").toInteger(),
        o.value("predictedEnergyMwh").toInteger(),
        o.value("predictedIdleCount").toInt(),
        (o.value("peakFlag").isBool()
             ? (o.value("peakFlag").toBool() ? QStringLiteral("PEAK") : QStringLiteral("NORMAL"))
             : o.value("peakFlag").toString()),
        o.value("staleFlag").toBool()};
}
QList<RevenuePoint> groupRevenueByDay(const QJsonArray& items)
{
    QMap<QString, RevenuePoint> grouped;
    for (const auto& v : items)
    {
        auto p = revenueFromJson(v.toObject());
        const auto key = dateTimeText(p.bucketStart, "yyyy-MM-dd");
        auto& day = grouped[key];
        day.bucketStart = p.bucketStart;
        day.amountCent += p.amountCent;
        day.orders += p.orders;
    }
    return grouped.values();
}
} // namespace ncs::admin
