#include "admin_main_window.h"

#include "admin_api_client.h"
#include "admin_main_window_utils.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QJsonObject>
#include <QNetworkReply>
#include <QPushButton>
#include <QSpinBox>

#include <algorithm>

namespace ncs::admin
{

void AdminMainWindow::addStation()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("新增充电站"));
    auto* form = new QFormLayout(&dialog);
    auto* code = new QLineEdit(&dialog);
    auto* name = new QLineEdit(&dialog);
    auto* address = new QLineEdit(&dialog);
    auto* adcode = new QLineEdit(&dialog);
    auto* latitude = new QSpinBox(&dialog);
    auto* longitude = new QSpinBox(&dialog);
    auto* businessHours = new QLineEdit(&dialog);
    auto* count = new QSpinBox(&dialog);
    auto* chargerType = new QComboBox(&dialog);
    auto* powerWatt = new QSpinBox(&dialog);
    auto* connectorStandard = new QLineEdit(&dialog);
    code->setPlaceholderText(QStringLiteral("例如 ZGC2"));
    adcode->setPlaceholderText(QStringLiteral("例如 110108"));
    latitude->setRange(-90000000, 90000000);
    longitude->setRange(-180000000, 180000000);
    latitude->setValue(39977680);
    longitude->setValue(116316417);
    businessHours->setText(QStringLiteral("00:00-24:00"));
    count->setRange(1, 100);
    count->setValue(4);
    chargerType->addItem(QStringLiteral("慢充"), 0);
    chargerType->addItem(QStringLiteral("快充"), 1);
    powerWatt->setRange(1000, 250000);
    powerWatt->setValue(60000);
    connectorStandard->setText(QStringLiteral("GB/T 20234.3"));
    adcode->setText(QStringLiteral("110108"));
    form->addRow(QStringLiteral("站点编码"), code);
    form->addRow(QStringLiteral("站点名称"), name);
    form->addRow(QStringLiteral("地址"), address);
    form->addRow(QStringLiteral("行政区编码"), adcode);
    form->addRow(QStringLiteral("纬度(E6)"), latitude);
    form->addRow(QStringLiteral("经度(E6)"), longitude);
    form->addRow(QStringLiteral("营业时间"), businessHours);
    form->addRow(QStringLiteral("初始桩数"), count);
    form->addRow(QStringLiteral("初始桩类型"), chargerType);
    form->addRow(QStringLiteral("单桩功率(W)"), powerWatt);
    form->addRow(QStringLiteral("接口标准"), connectorStandard);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    if (code->text().trimmed().isEmpty() || name->text().trimmed().isEmpty() ||
        address->text().trimmed().isEmpty() || adcode->text().trimmed().isEmpty() ||
        businessHours->text().trimmed().isEmpty() ||
        connectorStandard->text().trimmed().isEmpty()) {
        showApiError(QStringLiteral("站点编码、名称、地址、行政区编码、营业时间和接口标准不能为空"));
        return;
    }
    QJsonObject initialCharger;
    initialCharger.insert(QStringLiteral("count"), count->value());
    initialCharger.insert(QStringLiteral("chargerType"), chargerType->currentData().toInt());
    initialCharger.insert(QStringLiteral("powerWatt"), powerWatt->value());
    initialCharger.insert(QStringLiteral("connectorStandard"), connectorStandard->text().trimmed());

    QJsonObject body;
    body.insert(QStringLiteral("code"), code->text().trimmed());
    body.insert(QStringLiteral("name"), name->text().trimmed());
    body.insert(QStringLiteral("address"), address->text().trimmed());
    body.insert(QStringLiteral("adcode"), adcode->text().trimmed());
    body.insert(QStringLiteral("latitudeE6"), latitude->value());
    body.insert(QStringLiteral("longitudeE6"), longitude->value());
    body.insert(QStringLiteral("businessHours"), businessHours->text().trimmed());
    body.insert(QStringLiteral("initialCharger"), initialCharger);

    auto* reply = apiClient_->postJson(QStringLiteral("admin/stations"), body, true);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const auto bodyBytes = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            statusBar()->showMessage(requestFailureText(reply, bodyBytes), 6000);
            reply->deleteLater();
            return;
        }
        QString error;
        extractEnvelopeObject(bodyBytes, &error);
        if (!error.isEmpty()) {
            statusBar()->showMessage(error, 6000);
            reply->deleteLater();
            return;
        }
        refreshAllData();
        statusBar()->showMessage(QStringLiteral("充电站已提交到后端。"), 3000);
        reply->deleteLater();
    });
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
    const auto confirm = QMessageBox::question(
        this, QStringLiteral("停用站点"),
        QStringLiteral("确认停用站点 %1 吗？").arg(it->name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (confirm != QMessageBox::Yes) return;

    QJsonObject body;
    body.insert(QStringLiteral("reason"), QStringLiteral("界面停用站点"));
    body.insert(QStringLiteral("version"), it->version);
    auto* reply = apiClient_->postJson(QStringLiteral("admin/stations/%1/disable").arg(id), body,
                                       true);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const auto bodyBytes = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            statusBar()->showMessage(requestFailureText(reply, bodyBytes), 6000);
            reply->deleteLater();
            return;
        }
        QString error;
        extractEnvelopeObject(bodyBytes, &error);
        if (!error.isEmpty()) {
            statusBar()->showMessage(error, 6000);
            reply->deleteLater();
            return;
        }
        refreshAllData();
        statusBar()->showMessage(QStringLiteral("站点已提交停用请求。"), 3000);
        reply->deleteLater();
    });
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
    const QStringList states = {QStringLiteral("空闲"), QStringLiteral("使用中"),
                                QStringLiteral("故障"), QStringLiteral("已停用")};
    bool ok = false;
    int currentIndex = states.indexOf(it->status);
    if (currentIndex < 0) currentIndex = 0;
    const auto selected = QInputDialog::getItem(this, QStringLiteral("修改电桩状态"),
                                                QStringLiteral("状态"), states,
                                                currentIndex, false, &ok);
    if (!ok) return;
    int targetStatus = 0;
    if (selected == QStringLiteral("使用中")) targetStatus = 1;
    if (selected == QStringLiteral("故障")) targetStatus = 2;
    else if (selected == QStringLiteral("已停用")) targetStatus = 3;

    QJsonObject body;
    body.insert(QStringLiteral("targetStatus"), targetStatus);
    body.insert(QStringLiteral("reason"), QStringLiteral("界面修改电桩状态"));
    body.insert(QStringLiteral("version"), it->version);
    auto* reply = apiClient_->putJson(QStringLiteral("admin/chargers/%1/status").arg(it->id), body,
                                      true);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const auto bodyBytes = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            statusBar()->showMessage(requestFailureText(reply, bodyBytes), 6000);
            reply->deleteLater();
            return;
        }
        QString error;
        extractEnvelopeObject(bodyBytes, &error);
        if (!error.isEmpty()) {
            statusBar()->showMessage(error, 6000);
            reply->deleteLater();
            return;
        }
        refreshAllData();
        statusBar()->showMessage(QStringLiteral("电桩状态已提交到后端。"), 3000);
        reply->deleteLater();
    });
}

void AdminMainWindow::toggleUserStatus()
{
    const int row = userTable_->currentRow();
    if (row < 0) {
        showApiError(QStringLiteral("请先选择一个用户"));
        return;
    }
    const int id = userTable_->item(row, 0)->text().toInt();
    const auto it = std::find_if(users_.begin(), users_.end(),
                                 [id](const User& user) { return user.id == id; });
    if (it == users_.end()) return;

    const int targetStatus = it->status == QStringLiteral("冻结") ? 1 : 0;
    QJsonObject body;
    body.insert(QStringLiteral("status"), targetStatus);
    body.insert(QStringLiteral("reason"), QStringLiteral("界面修改用户状态"));
    body.insert(QStringLiteral("version"), it->version);
    auto* reply = apiClient_->putJson(QStringLiteral("admin/users/%1/status").arg(id), body,
                                      true);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const auto bodyBytes = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            statusBar()->showMessage(requestFailureText(reply, bodyBytes), 6000);
            reply->deleteLater();
            return;
        }
        QString error;
        extractEnvelopeObject(bodyBytes, &error);
        if (!error.isEmpty()) {
            statusBar()->showMessage(error, 6000);
            reply->deleteLater();
            return;
        }
        refreshUsers();
        statusBar()->showMessage(QStringLiteral("用户状态已提交到后端。"), 3000);
        reply->deleteLater();
    });
}

void AdminMainWindow::restartCharger()
{
    const int row = chargerTable_->currentRow();
    if (row < 0) {
        showApiError(QStringLiteral("请先选择一个电桩"));
        return;
    }
    const int id = chargerTable_->item(row, 0)->text().toInt();
    const auto it = std::find_if(chargers_.begin(), chargers_.end(),
                                 [id](const Charger& charger) { return charger.id == id; });
    if (it == chargers_.end()) return;

    bool ok = false;
    const auto reason = QInputDialog::getText(this, QStringLiteral("远程重启"),
                                              QStringLiteral("请输入重启原因"),
                                              QLineEdit::Normal,
                                              QStringLiteral("远程恢复测试"), &ok).trimmed();
    if (!ok || reason.isEmpty()) return;

    QJsonObject body;
    body.insert(QStringLiteral("confirm"), true);
    body.insert(QStringLiteral("reason"), reason);
    auto* reply = apiClient_->postJson(
        QStringLiteral("admin/chargers/%1/restart-commands").arg(it->id), body, true);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const auto bodyBytes = reply->readAll();
        const auto failure = requestFailureText(reply, bodyBytes);
        if (!failure.isEmpty()) {
            statusBar()->showMessage(failure, 6000);
            reply->deleteLater();
            return;
        }
        refreshAllData();
        statusBar()->showMessage(QStringLiteral("已向后端提交重启请求。"), 3000);
        reply->deleteLater();
    });
}

} // namespace ncs::admin
