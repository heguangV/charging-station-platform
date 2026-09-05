#include "admin_main_window.h"

#include "admin_api_client.h"
#include "admin_main_window_utils.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkReply>
#include <QStatusBar>
#include <QTableWidgetItem>
#include <QUrlQuery>

#include <numeric>

namespace ncs::admin
{

void AdminMainWindow::fillStationTable()
{
    if (!stationTable_) return;
    stationTable_->setRowCount(0);
    const auto keyword = stationSearch_ ? stationSearch_->text().trimmed() : QString();
    for (const auto& station : stations_) {
        if (!keyword.isEmpty() && !station.name.contains(keyword, Qt::CaseInsensitive) &&
            !station.address.contains(keyword, Qt::CaseInsensitive)) continue;
        const int row = stationTable_->rowCount();
        stationTable_->insertRow(row);
        stationTable_->setItem(row, 0, new QTableWidgetItem(QString::number(station.id)));
        stationTable_->setItem(row, 1, new QTableWidgetItem(station.name));
        stationTable_->setItem(row, 2, new QTableWidgetItem(station.address));
        stationTable_->setItem(row, 3,
                               new QTableWidgetItem(QStringLiteral("¥%1").arg(station.price, 0, 'f', 2)));
        stationTable_->setItem(row, 4, new QTableWidgetItem(QString::number(station.totalChargers)));
        stationTable_->setItem(row, 5, new QTableWidgetItem(QString::number(station.idleChargers)));
    }
}

void AdminMainWindow::fillChargerTable()
{
    if (!chargerTable_) return;
    chargerTable_->setRowCount(0);
    const auto keyword = chargerSearch_ ? chargerSearch_->text().trimmed() : QString();
    const auto state = chargerStatus_ ? chargerStatus_->currentText() : QStringLiteral("全部状态");
    for (const auto& charger : chargers_) {
        if (!keyword.isEmpty() && !charger.code.contains(keyword, Qt::CaseInsensitive) &&
            !charger.stationName.contains(keyword, Qt::CaseInsensitive)) continue;
        if (state != QStringLiteral("全部状态") && charger.status != state) continue;
        const int row = chargerTable_->rowCount();
        chargerTable_->insertRow(row);
        chargerTable_->setItem(row, 0, new QTableWidgetItem(charger.code));
        chargerTable_->setItem(row, 1, new QTableWidgetItem(charger.stationName));
        chargerTable_->setItem(row, 2, new QTableWidgetItem(charger.type));
        chargerTable_->setItem(row, 3, new QTableWidgetItem(QStringLiteral("%1 kW").arg(charger.power)));
        chargerTable_->setItem(row, 4, new QTableWidgetItem(charger.status));
        chargerTable_->setItem(row, 5, new QTableWidgetItem(QString::number(charger.totalCount)));
    }
}

void AdminMainWindow::fillUserTable()
{
    if (!userTable_) return;
    userTable_->setRowCount(0);
    const auto keyword = userSearch_ ? userSearch_->text().trimmed() : QString();
    for (const auto& user : users_) {
        if (!keyword.isEmpty() && !user.phone.contains(keyword, Qt::CaseInsensitive) &&
            !user.nickname.contains(keyword, Qt::CaseInsensitive)) continue;
        const int row = userTable_->rowCount();
        userTable_->insertRow(row);
        userTable_->setItem(row, 0, new QTableWidgetItem(QString::number(user.id)));
        userTable_->setItem(row, 1, new QTableWidgetItem(user.phone));
        userTable_->setItem(row, 2, new QTableWidgetItem(user.nickname));
        userTable_->setItem(row, 3, new QTableWidgetItem(QStringLiteral("¥%1").arg(user.balance)));
        userTable_->setItem(row, 4, new QTableWidgetItem(user.status));
    }
}

void AdminMainWindow::refreshStations()
{
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("page"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("pageSize"), QStringLiteral("1000"));
    const auto keyword = stationSearch_ ? stationSearch_->text().trimmed() : QString();
    if (!keyword.isEmpty()) query.addQueryItem(QStringLiteral("keyword"), keyword);

    auto* reply = apiClient_->get(QStringLiteral("admin/stations"), query);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const auto bodyBytes = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            statusBar()->showMessage(requestFailureText(reply, bodyBytes), 6000);
            reply->deleteLater();
            return;
        }
        QString error;
        const auto payload = extractPayload(bodyBytes, &error);
        if (!error.isEmpty()) {
            statusBar()->showMessage(error, 6000);
            reply->deleteLater();
            return;
        }
        stations_.clear();
        for (const auto& item : objectsFromValue(payload)) stations_.append(stationFromJson(item));
        fillStationTable();
        reply->deleteLater();
    });
}

void AdminMainWindow::refreshChargers()
{
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("page"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("pageSize"), QStringLiteral("1000"));
    const auto keyword = chargerSearch_ ? chargerSearch_->text().trimmed() : QString();
    if (!keyword.isEmpty()) query.addQueryItem(QStringLiteral("keyword"), keyword);

    auto* reply = apiClient_->get(QStringLiteral("admin/chargers"), query);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const auto bodyBytes = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            statusBar()->showMessage(requestFailureText(reply, bodyBytes), 6000);
            reply->deleteLater();
            return;
        }
        QString error;
        const auto payload = extractPayload(bodyBytes, &error);
        if (!error.isEmpty()) {
            statusBar()->showMessage(error, 6000);
            reply->deleteLater();
            return;
        }
        chargers_.clear();
        for (const auto& item : objectsFromValue(payload)) chargers_.append(chargerFromJson(item));
        fillChargerTable();
        reply->deleteLater();
    });
}

void AdminMainWindow::refreshUsers()
{
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("page"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("pageSize"), QStringLiteral("1000"));

    auto* reply = apiClient_->get(QStringLiteral("admin/users"), query);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const auto bodyBytes = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            statusBar()->showMessage(requestFailureText(reply, bodyBytes), 6000);
            reply->deleteLater();
            return;
        }
        QString error;
        const auto payload = extractPayload(bodyBytes, &error);
        if (!error.isEmpty()) {
            statusBar()->showMessage(error, 6000);
            reply->deleteLater();
            return;
        }
        users_.clear();
        for (const auto& item : objectsFromValue(payload)) users_.append(userFromJson(item));
        fillUserTable();
        if (dashboardRegisteredUser_ != nullptr) {
            dashboardRegisteredUser_->setText(QString::number(users_.size()));
        }
        reply->deleteLater();
    });
}

void AdminMainWindow::refreshOverview()
{
    auto* revenueReply = apiClient_->get(QStringLiteral("admin/stats/revenue"));
    connect(revenueReply, &QNetworkReply::finished, this, [this, revenueReply] {
        const auto bodyBytes = revenueReply->readAll();
        if (revenueReply->error() != QNetworkReply::NoError) {
            statusBar()->showMessage(requestFailureText(revenueReply, bodyBytes), 6000);
            revenueReply->deleteLater();
            return;
        }
        QString error;
        const auto payload = extractPayload(bodyBytes, &error);
        if (!error.isEmpty()) {
            statusBar()->showMessage(error, 6000);
            revenueReply->deleteLater();
            return;
        }
        revenue_.clear();
        for (const auto& item : objectsFromValue(payload)) revenue_.append(revenueFromJson(item));
        if (dashboardRevenueTable_ != nullptr) {
            dashboardRevenueTable_->setRowCount(0);
            for (const auto& point : revenue_) {
                const int row = dashboardRevenueTable_->rowCount();
                dashboardRevenueTable_->insertRow(row);
                dashboardRevenueTable_->setItem(row, 0, new QTableWidgetItem(point.date));
                dashboardRevenueTable_->setItem(
                    row, 1, new QTableWidgetItem(QStringLiteral("¥%1").arg(point.revenue)));
                dashboardRevenueTable_->setItem(row, 2, new QTableWidgetItem(QString::number(point.orders)));
            }
        }
        if (!revenue_.isEmpty()) {
            const double monthRevenue = std::accumulate(
                revenue_.cbegin(), revenue_.cend(), 0.0,
                [](double sum, const RevenuePoint& point) { return sum + point.revenue.toDouble(); });
            if (dashboardTodayRevenue_ != nullptr) {
                dashboardTodayRevenue_->setText(QStringLiteral("¥%1").arg(revenue_.first().revenue));
            }
            if (dashboardMonthRevenue_ != nullptr) {
                dashboardMonthRevenue_->setText(QStringLiteral("¥%1").arg(monthRevenue, 0, 'f', 2));
            }
        }
        revenueReply->deleteLater();
    });

    auto* chargerReply = apiClient_->get(QStringLiteral("admin/stats/charger-status"));
    connect(chargerReply, &QNetworkReply::finished, this, [this, chargerReply] {
        const auto bodyBytes = chargerReply->readAll();
        if (chargerReply->error() != QNetworkReply::NoError) {
            statusBar()->showMessage(requestFailureText(chargerReply, bodyBytes), 6000);
            chargerReply->deleteLater();
            return;
        }
        QString error;
        const auto payload = extractPayload(bodyBytes, &error);
        if (!error.isEmpty() || !payload.isObject()) {
            statusBar()->showMessage(error.isEmpty() ? QStringLiteral("设备状态统计解析失败") : error, 6000);
            chargerReply->deleteLater();
            return;
        }
        const auto object = payload.toObject();
        const int idle = firstInt(object, {"idle", "free", "idleCount"});
        const int inUse = firstInt(object, {"inUse", "busy", "using"});
        const int fault = firstInt(object, {"fault", "failed"});
        const int total = firstInt(object, {"total", "count"}, idle + inUse + fault);
        const double health = total > 0 ? (idle + inUse) * 100.0 / total : 0.0;
        if (dashboardIdleCharger_ != nullptr) {
            dashboardIdleCharger_->setText(QStringLiteral("空闲    %1").arg(idle));
        }
        if (dashboardInUseCharger_ != nullptr) {
            dashboardInUseCharger_->setText(QStringLiteral("使用中  %1").arg(inUse));
        }
        if (dashboardFaultCharger_ != nullptr) {
            dashboardFaultCharger_->setText(QStringLiteral("故障    %1").arg(fault));
        }
        if (dashboardHealthScore_ != nullptr) {
            dashboardHealthScore_->setText(QStringLiteral("设备健康度  %1%").arg(health, 0, 'f', 1));
        }
        if (dashboardOnlineCharger_ != nullptr) {
            dashboardOnlineCharger_->setText(QStringLiteral("%1 / %2").arg(idle + inUse).arg(total));
        }
        chargerReply->deleteLater();
    });
}

void AdminMainWindow::refreshPredictions()
{
    const int stationId = stations_.isEmpty() ? 1 : stations_.first().id;
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("stationId"), QString::number(stationId));
    query.addQueryItem(QStringLiteral("horizonHour"), QStringLiteral("24"));

    auto* reply = apiClient_->get(QStringLiteral("admin/predictions"), query);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const auto bodyBytes = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            statusBar()->showMessage(requestFailureText(reply, bodyBytes), 6000);
            reply->deleteLater();
            return;
        }
        QString error;
        const auto payload = extractPayload(bodyBytes, &error);
        if (!error.isEmpty()) {
            statusBar()->showMessage(error, 6000);
            reply->deleteLater();
            return;
        }
        predictions_.clear();
        for (const auto& item : objectsFromValue(payload)) predictions_.append(predictionFromJson(item));
        if (predictionTable_ != nullptr) {
            predictionTable_->setRowCount(0);
            for (const auto& point : predictions_) {
                const int row = predictionTable_->rowCount();
                predictionTable_->insertRow(row);
                predictionTable_->setItem(row, 0, new QTableWidgetItem(point.targetTime));
                predictionTable_->setItem(row, 1, new QTableWidgetItem(point.stationName));
                predictionTable_->setItem(row, 2, new QTableWidgetItem(point.energy));
                predictionTable_->setItem(row, 3, new QTableWidgetItem(QString::number(point.freeCount)));
                predictionTable_->setItem(row, 4, new QTableWidgetItem(point.peakFlag));
            }
        }
        reply->deleteLater();
    });
}

} // namespace ncs::admin
