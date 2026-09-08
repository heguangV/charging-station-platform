#include "admin_charts.h"
#include "admin_main_window.h"
#include "admin_main_window_utils.h"
#include "admin_page_status.h"
#include <QComboBox>
#include <QDateTime>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTableWidget>
#include <memory>
namespace ncs::admin
{
namespace
{
void cell(QTableWidget* table, int row, int col, const QString& text, qint64 id = 0)
{
    auto* item = new QTableWidgetItem(text);
    item->setData(Qt::UserRole, id);
    item->setToolTip(QStringLiteral("<qt>%1</qt>").arg(text.toHtmlEscaped()));
    table->setItem(row, col, item);
}
void statusCell(QTableWidget* table, int row, int col, const QString& text, int status,
                qint64 id = 0)
{
    cell(table, row, col, text, id);
    table->item(row, col)->setForeground(status == 2   ? QColor("#B42318")
                                         : status == 3 ? QColor("#75828B")
                                         : status == 4 ? QColor("#9C781D")
                                                       : QColor("#23794E"));
}
QUrlQuery pageQuery(int page)
{
    QUrlQuery q;
    q.addQueryItem("page", QString::number(page));
    q.addQueryItem("pageSize", "50");
    return q;
}
bool objects(const QJsonObject& data)
{
    if (!data.value("items").isArray())
        return false;
    for (const auto& item : data.value("items").toArray())
        if (!item.isObject())
            return false;
    return true;
}
} // namespace
quint64 AdminMainWindow::beginPage(int page)
{
    states_[page]->loading();
    if (previous_[page])
    {
        previous_[page]->setEnabled(false);
        next_[page]->setEnabled(false);
    }
    switch (page)
    {
    case 0:
        for (auto* l : {todayRevenue_, monthRevenue_, operationalChargers_, registeredUsers_})
            l->setText(QStringLiteral("—"));
        health_->setText(QStringLiteral("健康度 —"));
        trend_->hide();
        statusChart_->hide();
        revenueTable_->setRowCount(0);
        break;
    case 1:
        stations_.clear();
        stationTable_->setRowCount(0);
        break;
    case 2:
        chargers_.clear();
        chargerTable_->setRowCount(0);
        break;
    case 3:
        users_.clear();
        userTable_->setRowCount(0);
        break;
    case 4:
        predictions_.clear();
        predictionTable_->setRowCount(0);
        break;
    }
    return ++generations_[page];
}
void AdminMainWindow::failPage(int page, const QString& message)
{
    states_[page]->failed(message);
    if (previous_[page])
        previous_[page]->setEnabled(pageNumbers_[page] > 1);
    connectionLabel_->setText(QStringLiteral("请求未完成，可重试"));
}
void AdminMainWindow::loadList(int page, const QString& path, const QUrlQuery& query,
                               std::function<void(const QJsonObject&)> done)
{
    if (!api_.hasSession())
        return;
    const auto generation = beginPage(page);
    api_.get(path, query,
             [this, page, generation, done = std::move(done)](AdminReply reply)
             {
                 if (generation != generations_[page])
                     return;
                 if (!reply.ok())
                 {
                     failPage(page, reply.message);
                     return;
                 }
                 if (!reply.data.isObject() || !objects(reply.data.toObject()))
                 {
                     failPage(page, QStringLiteral("列表响应格式错误，请重试"));
                     return;
                 }
                 connectionLabel_->setText(QStringLiteral("服务已连接"));
                 done(reply.data.toObject());
             });
}
QString AdminMainWindow::stationName(qint64 id) const
{
    for (const auto& s : catalog_)
        if (s.id == id)
            return s.name;
    return QStringLiteral("站点 #%1").arg(id);
}
void AdminMainWindow::loadCatalog(int page, QList<Station> accumulated)
{
    if (!api_.hasSession() || (page == 1 && catalogLoading_))
        return;
    catalogLoading_ = true;
    const auto session = sessionGeneration_;
    QUrlQuery q;
    q.addQueryItem("page", QString::number(page));
    q.addQueryItem("pageSize", "100");
    api_.get("admin/stations", q,
             [this, page, session, accumulated = std::move(accumulated)](AdminReply reply) mutable
             {
                 if (session != sessionGeneration_)
                     return;
                 if (!reply.ok() || !reply.data.isObject() || !objects(reply.data.toObject()))
                 {
                     catalogLoading_ = false;
                     notify(
                         QStringLiteral("站点目录加载失败，部分名称暂以站点 ID 显示；可刷新重试"),
                         true);
                     return;
                 }
                 const auto o = reply.data.toObject();
                 const auto items = o.value("items").toArray();
                 for (const auto& v : items)
                     accumulated.append(stationFromJson(v.toObject()));
                 const int total = o.value("total").toInt();
                 if (accumulated.size() < total)
                 {
                     if (items.isEmpty() || page >= 500)
                     {
                         catalogLoading_ = false;
                         notify(QStringLiteral("站点目录未完整返回，请重试"), true);
                         return;
                     }
                     loadCatalog(page + 1, std::move(accumulated));
                     return;
                 }
                 catalog_ = std::move(accumulated);
                 catalogLoading_ = false;
                 updateStationChoices();
                 fillChargerTable();
                 for (int row = 0; row < predictions_.size(); ++row)
                     cell(predictionTable_, row, 1, stationName(predictions_[row].stationId));
             });
}
void AdminMainWindow::updateStationChoices()
{
    for (auto* combo : {chargerStation_, predictionStation_})
    {
        const qint64 selected = combo->currentData().toLongLong();
        const QSignalBlocker blocker(combo);
        combo->clear();
        combo->addItem(QStringLiteral("全部站点"), qint64(0));
        for (const auto& s : catalog_)
            combo->addItem(s.name, s.id);
        combo->setCurrentIndex(qMax(0, combo->findData(selected)));
    }
}
void AdminMainWindow::fillStationTable()
{
    stationTable_->setRowCount(0);
    for (const auto& s : stations_)
    {
        const int row = stationTable_->rowCount();
        stationTable_->insertRow(row);
        cell(stationTable_, row, 0, s.code, s.id);
        cell(stationTable_, row, 1, s.name);
        cell(stationTable_, row, 2, s.adcode);
        statusCell(stationTable_, row, 3,
                   s.enabled ? QStringLiteral("运营中") : QStringLiteral("已停用"),
                   s.enabled ? 0 : 3);
    }
}
void AdminMainWindow::fillChargerTable()
{
    chargerTable_->setRowCount(0);
    for (const auto& c : chargers_)
    {
        const int row = chargerTable_->rowCount();
        chargerTable_->insertRow(row);
        cell(chargerTable_, row, 0, c.code, c.id);
        cell(chargerTable_, row, 1, stationName(c.stationId));
        cell(chargerTable_, row, 2, c.type);
        cell(chargerTable_, row, 3, QStringLiteral("%1 kW").arg(c.powerWatt / 1000.0, 0, 'f', 1));
        statusCell(chargerTable_, row, 4, c.status, c.statusCode);
        cell(chargerTable_, row, 5, QString::number(c.totalCount));
        cell(chargerTable_, row, 6,
             QStringLiteral("%1 小时").arg(c.totalMinutes / 60.0, 0, 'f', 1));
    }
}
void AdminMainWindow::fillUserTable()
{
    userTable_->setRowCount(0);
    for (const auto& u : users_)
    {
        const int row = userTable_->rowCount();
        userTable_->insertRow(row);
        cell(userTable_, row, 0, QString::number(u.id), u.id);
        cell(userTable_, row, 1, u.phone);
        cell(userTable_, row, 2, u.nickname);
        cell(userTable_, row, 3, money(u.balanceCent));
        cell(userTable_, row, 4, dateTimeText(u.registeredAt));
        statusCell(userTable_, row, 5, u.status, u.statusCode == 0 ? 2 : 0);
    }
}
void AdminMainWindow::refreshStations()
{
    auto q = pageQuery(pageNumbers_[1]);
    const auto keyword = stationSearch_->text().trimmed();
    if (!keyword.isEmpty())
        q.addQueryItem("keyword", keyword);
    loadList(1, "admin/stations", q,
             [this](const QJsonObject& o)
             {
                 for (const auto& v : o.value("items").toArray())
                     stations_.append(stationFromJson(v.toObject()));
                 fillStationTable();
                 updatePager(1, o.value("total").toInt());
                 states_[1]->ready(stations_.isEmpty()
                                       ? QStringLiteral("没有匹配的站点，可调整搜索条件")
                                       : QStringLiteral("选择站点可查看设备或变更运营状态"));
             });
}
void AdminMainWindow::refreshChargers()
{
    auto q = pageQuery(pageNumbers_[2]);
    if (!chargerSearch_->text().trimmed().isEmpty())
        q.addQueryItem("keyword", chargerSearch_->text().trimmed());
    if (chargerStation_->currentData().toLongLong() > 0)
        q.addQueryItem("stationId", chargerStation_->currentData().toString());
    if (chargerStatus_->currentData().toInt() >= 0)
        q.addQueryItem("status", chargerStatus_->currentData().toString());
    loadList(2, "admin/chargers", q,
             [this](const QJsonObject& o)
             {
                 for (const auto& v : o.value("items").toArray())
                     chargers_.append(chargerFromJson(v.toObject()));
                 fillChargerTable();
                 updatePager(2, o.value("total").toInt());
                 states_[2]->ready(chargers_.isEmpty()
                                       ? QStringLiteral("没有匹配的电桩，可调整筛选条件")
                                       : QStringLiteral("选择电桩后可修改状态或发起维护"));
             });
}
void AdminMainWindow::refreshUsers()
{
    const auto text = userSearch_->text().trimmed();
    if (!text.isEmpty() &&
        !QRegularExpression(QStringLiteral("^([0-9]{4}|[0-9]{11})$")).match(text).hasMatch())
    {
        beginPage(3);
        failPage(3, QStringLiteral("请输入完整的 11 位手机号或后四位"));
        return;
    }
    auto q = pageQuery(pageNumbers_[3]);
    if (!text.isEmpty())
        q.addQueryItem(text.size() == 4 ? "phoneLast4" : "phoneExact", text);
    loadList(3, "admin/users", q,
             [this](const QJsonObject& o)
             {
                 for (const auto& v : o.value("items").toArray())
                     users_.append(userFromJson(v.toObject()));
                 fillUserTable();
                 updatePager(3, o.value("total").toInt());
                 states_[3]->ready(
                     users_.isEmpty()
                         ? QStringLiteral("未找到匹配用户")
                         : QStringLiteral("手机号已脱敏，冻结前会再次确认用户最新状态"));
             });
}
void AdminMainWindow::refreshPredictions()
{
    QUrlQuery q;
    q.addQueryItem("horizonHour", predictionHorizon_->currentData().toString());
    if (predictionStation_->currentData().toLongLong() > 0)
        q.addQueryItem("stationId", predictionStation_->currentData().toString());
    loadList(4, "admin/predictions", q,
             [this](const QJsonObject& o)
             {
                 for (const auto& v : o.value("items").toArray())
                     predictions_.append(predictionFromJson(v.toObject()));
                 for (const auto& p : predictions_)
                 {
                     const int row = predictionTable_->rowCount();
                     predictionTable_->insertRow(row);
                     cell(predictionTable_, row, 0, dateTimeText(p.targetAt));
                     cell(predictionTable_, row, 1, stationName(p.stationId));
                     cell(predictionTable_, row, 2,
                          QStringLiteral("%1 kWh").arg(p.energyMwh / 1000000.0, 0, 'f', 2));
                     cell(predictionTable_, row, 3, QString::number(p.idleCount));
                     statusCell(predictionTable_, row, 4, peakText(p.peakFlag),
                                p.peakFlag == "PEAK" ? 2 : 0);
                     statusCell(predictionTable_, row, 5,
                                p.stale ? QStringLiteral("已过期") : QStringLiteral("有效"),
                                p.stale ? 4 : 0);
                 }
                 states_[4]->ready(predictions_.isEmpty()
                                       ? QStringLiteral("暂无预测结果，可运行预测任务")
                                       : QStringLiteral("预测仅供运营参考，请关注过期标记"));
             });
}
void AdminMainWindow::refreshOverview()
{
    if (!api_.hasSession())
        return;
    const auto generation = beginPage(0);
    const auto now = QDateTime::currentDateTimeUtc().toTimeZone(businessTimeZone());
    const auto day = QDateTime(now.date(), QTime(0, 0), businessTimeZone());
    const auto month =
        QDateTime(QDate(now.date().year(), now.date().month(), 1), QTime(0, 0), businessTimeZone());
    const int days = revenueRange_->currentData().toInt();
    const auto from = day.addDays(1 - days);
    struct State
    {
        int pending = 5;
        QStringList errors;
    };
    auto state = std::make_shared<State>();
    auto fetch = [this, generation, state](const QString& path, const QUrlQuery& query,
                                           std::function<bool(const QJsonObject&)> render)
    {
        api_.get(
            path, query,
            [this, generation, state, render = std::move(render)](AdminReply reply)
            {
                if (generation != generations_[0])
                    return;
                if (!reply.ok())
                    state->errors.append(reply.message);
                else if (!reply.data.isObject() || !render(reply.data.toObject()))
                    state->errors.append(QStringLiteral("统计响应字段不完整"));
                if (--state->pending == 0)
                {
                    if (state->errors.isEmpty())
                    {
                        states_[0]->ready(
                            QStringLiteral("已更新 · %1")
                                .arg(dateTimeText(QDateTime::currentSecsSinceEpoch(), "HH:mm:ss")));
                        connectionLabel_->setText(QStringLiteral("服务已连接"));
                    }
                    else
                    {
                        state->errors.removeDuplicates();
                        failPage(0, state->errors.join(QStringLiteral("；")));
                    }
                }
            });
    };
    fetch("admin/stats/revenue", revenueQuery(day.toSecsSinceEpoch(), now.toSecsSinceEpoch()),
          [this](const QJsonObject& o)
          {
              if (!o.value("totalAmountCent").isDouble())
                  return false;
              todayRevenue_->setText(money(o.value("totalAmountCent").toInteger()));
              return true;
          });
    fetch("admin/stats/revenue", revenueQuery(month.toSecsSinceEpoch(), now.toSecsSinceEpoch()),
          [this](const QJsonObject& o)
          {
              if (!o.value("totalAmountCent").isDouble())
                  return false;
              monthRevenue_->setText(money(o.value("totalAmountCent").toInteger()));
              return true;
          });
    fetch("admin/stats/revenue", revenueQuery(from.toSecsSinceEpoch(), now.toSecsSinceEpoch()),
          [this, from, days](const QJsonObject& o)
          {
              if (!objects(o))
                  return false;
              const auto points = groupRevenueByDay(o.value("items").toArray());
              QList<RevenuePoint> series;
              if (!points.isEmpty())
                  for (int i = 0; i < days; ++i)
                  {
                      RevenuePoint value{from.addDays(i).toSecsSinceEpoch(), 0, 0};
                      for (const auto& p : points)
                          if (dateTimeText(p.bucketStart, "yyyy-MM-dd") ==
                              dateTimeText(value.bucketStart, "yyyy-MM-dd"))
                          {
                              value.amountCent += p.amountCent;
                              value.orders += p.orders;
                          }
                      series.append(value);
                  }
              trend_->setPoints(series);
              trend_->show();
              for (const auto& p : series)
              {
                  int row = revenueTable_->rowCount();
                  revenueTable_->insertRow(row);
                  cell(revenueTable_, row, 0, dateTimeText(p.bucketStart, "yyyy-MM-dd"));
                  cell(revenueTable_, row, 1, money(p.amountCent));
                  cell(revenueTable_, row, 2, QString::number(p.orders));
              }
              return true;
          });
    fetch("admin/stats/charger-status", {},
          [this](const QJsonObject& o)
          {
              for (const auto* field :
                   {"idleCount", "occupiedCount", "faultyCount", "restartingCount", "disabledCount",
                    "operationalCount", "totalCount", "healthPercent"})
                  if (!o.value(field).isDouble())
                      return false;
              statusChart_->setCounts(
                  {o.value("idleCount").toInt(), o.value("occupiedCount").toInt(),
                   o.value("faultyCount").toInt(), o.value("restartingCount").toInt(),
                   o.value("disabledCount").toInt()});
              statusChart_->show();
              health_->setText(QStringLiteral("设备健康度  %1%")
                                   .arg(o.value("healthPercent").toDouble(), 0, 'f', 1));
              operationalChargers_->setText(QStringLiteral("%1 / %2")
                                                .arg(o.value("operationalCount").toInt())
                                                .arg(o.value("totalCount").toInt()));
              return true;
          });
    QUrlQuery users;
    users.addQueryItem("page", "1");
    users.addQueryItem("pageSize", "1");
    fetch("admin/users", users,
          [this](const QJsonObject& o)
          {
              if (!o.value("total").isDouble())
                  return false;
              registeredUsers_->setText(QString::number(o.value("total").toInt()));
              return true;
          });
}
} // namespace ncs::admin
