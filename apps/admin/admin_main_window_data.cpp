#include "admin_main_window.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QSpinBox>
#include <QStatusBar>
#include <QTableWidget>
#include <QTableWidgetItem>

#include <algorithm>

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
        stationTable_->setItem(row, 3, new QTableWidgetItem(QStringLiteral("¥%1").arg(station.price, 0, 'f', 2)));
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

void AdminMainWindow::refreshStations() { fillStationTable(); }
void AdminMainWindow::refreshChargers() { fillChargerTable(); }
void AdminMainWindow::refreshUsers() { fillUserTable(); }

void AdminMainWindow::addStation()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("新增充电站"));
    auto* form = new QFormLayout(&dialog);
    auto* name = new QLineEdit(&dialog);
    auto* address = new QLineEdit(&dialog);
    auto* price = new QDoubleSpinBox(&dialog);
    price->setRange(0.01, 99.99);
    price->setValue(1.50);
    price->setDecimals(2);
    auto* count = new QSpinBox(&dialog);
    count->setRange(1, 100);
    count->setValue(8);
    form->addRow(QStringLiteral("站点名称"), name);
    form->addRow(QStringLiteral("地址"), address);
    form->addRow(QStringLiteral("单价/度"), price);
    form->addRow(QStringLiteral("初始桩数"), count);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    if (name->text().trimmed().isEmpty() || address->text().trimmed().isEmpty()) {
        showApiError(QStringLiteral("站点名称和地址不能为空"));
        return;
    }
    const int id = stations_.isEmpty() ? 1 : stations_.last().id + 1;
    stations_.append({id, name->text().trimmed(), address->text().trimmed(), price->value(),
                      count->value(), count->value()});
    fillStationTable();
    statusBar()->showMessage(QStringLiteral("充电站已加入演示数据。"), 3000);
}

void AdminMainWindow::removeStation()
{
    const int row = stationTable_->currentRow();
    if (row < 0) {
        showApiError(QStringLiteral("请先选择一个充电站"));
        return;
    }
    const int id = stationTable_->item(row, 0)->text().toInt();
    const auto it = std::find_if(stations_.begin(), stations_.end(),
                                 [id](const Station& station) { return station.id == id; });
    if (it == stations_.end()) return;
    if (it->totalChargers > 0) {
        showApiError(QStringLiteral("该站下仍有电桩，禁止删除"));
        return;
    }
    stations_.erase(it);
    fillStationTable();
}

void AdminMainWindow::updateChargerStatus()
{
    const int row = chargerTable_->currentRow();
    if (row < 0) {
        showApiError(QStringLiteral("请先选择一个电桩"));
        return;
    }
    const QString code = chargerTable_->item(row, 0)->text();
    const auto it = std::find_if(chargers_.begin(), chargers_.end(),
                                 [&code](const Charger& charger) { return charger.code == code; });
    if (it == chargers_.end()) return;
    const QStringList states = {QStringLiteral("空闲"), QStringLiteral("故障")};
    bool ok = false;
    const auto selected = QInputDialog::getItem(this, QStringLiteral("修改电桩状态"),
                                                QStringLiteral("状态"), states,
                                                states.indexOf(it->status), false, &ok);
    if (!ok) return;
    it->status = selected;
    fillChargerTable();
}

void AdminMainWindow::toggleUserStatus()
{
    const int row = userTable_->currentRow();
    if (row < 0) {
        showApiError(QStringLiteral("请先选择一个用户"));
        return;
    }
    const int id = userTable_->item(row, 0)->text().toInt();
    for (auto& user : users_) {
        if (user.id == id)
            user.status = user.status == QStringLiteral("正常") ? QStringLiteral("冻结")
                                                                  : QStringLiteral("正常");
    }
    fillUserTable();
}

} // namespace ncs::admin
