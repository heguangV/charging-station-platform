#include "admin_main_window.h"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTimer>
#include <algorithm>
namespace ncs::admin
{
bool AdminMainWindow::confirmReason(const QString& title, const QString& description,
                                    QString* reason)
{
    QDialog dialog(this);
    dialog.setObjectName("confirmOperation");
    dialog.setWindowTitle(title);
    dialog.setMinimumWidth(420);
    auto* form = new QFormLayout(&dialog);
    form->setContentsMargins(24, 24, 24, 24);
    form->setSpacing(16);
    auto* hint = new QLabel(description);
    hint->setTextFormat(Qt::PlainText);
    hint->setWordWrap(true);
    form->addRow(hint);
    auto* input = new QLineEdit;
    input->setObjectName("operationReason");
    input->setMaxLength(200);
    input->setPlaceholderText(QStringLiteral("填写操作原因，至少 2 个字"));
    form->addRow(QStringLiteral("操作原因"), input);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确认操作"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    form->addRow(buttons);
    connect(
        input, &QLineEdit::textChanged, &dialog,
        [input, buttons] {
            buttons->button(QDialogButtonBox::Ok)->setEnabled(input->text().trimmed().size() >= 2);
        });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    *reason = input->text().trimmed();
    return api_.hasSession();
}
void AdminMainWindow::mutate(const QString& method, const QString& path, const QJsonObject& body,
                             int page)
{
    if (!api_.hasSession())
        return;
    setBusy(true);
    auto callback = [this, page](AdminReply reply)
    {
        setBusy(false);
        notify(reply.ok() ? QStringLiteral("操作已完成，列表正在更新") : reply.message,
               !reply.ok());
        if (reply.ok() || reply.httpStatus == 409)
        {
            if (page == 1)
            {
                catalog_.clear();
                loadCatalog();
                refreshStations();
            }
            else if (page == 2)
                refreshChargers();
            else if (page == 3)
                refreshUsers();
        }
    };
    if (method == "PUT")
        api_.putJson(path, body, std::move(callback), true);
    else
        api_.postJson(path, body, std::move(callback), true);
}
void AdminMainWindow::addStation()
{
    if (operationBusy_)
        return;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("新增充电站"));
    dialog.setMinimumWidth(470);
    auto* form = new QFormLayout(&dialog);
    form->setContentsMargins(24, 20, 24, 20);
    form->setSpacing(10);
    auto field = [&form](const QString& label, int max, const QString& value = QString())
    {
        auto* input = new QLineEdit(value);
        input->setMaxLength(max);
        form->addRow(label, input);
        return input;
    };
    auto* code = field(QStringLiteral("站点编码"), 16);
    auto* name = field(QStringLiteral("站点名称"), 64);
    auto* address = field(QStringLiteral("地址"), 128);
    auto* adcode = field(QStringLiteral("行政区编码"), 6);
    auto* latitude = new QDoubleSpinBox;
    latitude->setDecimals(6);
    latitude->setRange(-90, 90);
    latitude->setSuffix(QStringLiteral(" °"));
    auto* longitude = new QDoubleSpinBox;
    longitude->setDecimals(6);
    longitude->setRange(-180, 180);
    longitude->setSuffix(QStringLiteral(" °"));
    form->addRow(QStringLiteral("纬度"), latitude);
    form->addRow(QStringLiteral("经度"), longitude);
    auto* hours = field(QStringLiteral("营业时间"), 64, QStringLiteral("00:00-24:00"));
    auto* count = new QSpinBox;
    count->setRange(1, 100);
    count->setValue(4);
    form->addRow(QStringLiteral("初始电桩数量"), count);
    auto* type = new QComboBox;
    type->addItem(QStringLiteral("交流慢充"), 0);
    type->addItem(QStringLiteral("直流快充"), 1);
    type->setCurrentIndex(1);
    form->addRow(QStringLiteral("电桩类型"), type);
    auto* power = new QDoubleSpinBox;
    power->setDecimals(3);
    power->setRange(0.001, 1000);
    power->setValue(60);
    power->setSuffix(" kW");
    form->addRow(QStringLiteral("单桩功率"), power);
    auto* connector = field(QStringLiteral("接口标准"), 32, QStringLiteral("GB/T 20234.3"));
    auto* error = new QLabel;
    error->setWordWrap(true);
    error->setStyleSheet("color:#B42318;");
    form->addRow(error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("创建站点"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog,
            [&]
            {
                if (code->text().trimmed().size() < 2 || name->text().trimmed().isEmpty() ||
                    address->text().trimmed().isEmpty() ||
                    !QRegularExpression("^[0-9]{6}$").match(adcode->text()).hasMatch() ||
                    hours->text().trimmed().isEmpty() || connector->text().trimmed().isEmpty())
                {
                    error->setText(
                        QStringLiteral("请完整填写站点信息；编码至少 2 位，行政区编码为 6 位数字"));
                    return;
                }
                dialog.accept();
            });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted || !api_.hasSession())
        return;
    mutate("POST", "admin/stations",
           {{"code", code->text().trimmed()},
            {"name", name->text().trimmed()},
            {"address", address->text().trimmed()},
            {"adcode", adcode->text()},
            {"latitudeE6", qRound64(latitude->value() * 1000000)},
            {"longitudeE6", qRound64(longitude->value() * 1000000)},
            {"businessHours", hours->text().trimmed()},
            {"initialCharger", QJsonObject{{"count", count->value()},
                                           {"chargerType", type->currentData().toInt()},
                                           {"powerWatt", qRound64(power->value() * 1000)},
                                           {"connectorStandard", connector->text().trimmed()}}}},
           1);
}
void AdminMainWindow::removeStation()
{
    if (operationBusy_)
        return;
    const auto id = selectedId(stationTable_);
    const auto it = std::find_if(stations_.cbegin(), stations_.cend(),
                                 [id](const Station& s) { return s.id == id; });
    if (it == stations_.cend())
    {
        notify(QStringLiteral("请先选择站点"), true);
        return;
    }
    const auto station = *it;
    QString reason;
    if (!confirmReason(station.enabled ? QStringLiteral("停用站点") : QStringLiteral("启用站点"),
                       QStringLiteral("确认变更「%1」的运营状态？").arg(station.name), &reason))
        return;
    mutate("POST",
           QStringLiteral("admin/stations/%1/%2")
               .arg(station.id)
               .arg(station.enabled ? "disable" : "enable"),
           {{"version", station.version}, {"reason", reason}}, 1);
}
void AdminMainWindow::updateChargerStatus()
{
    if (operationBusy_)
        return;
    const auto id = selectedId(chargerTable_);
    const auto it = std::find_if(chargers_.cbegin(), chargers_.cend(),
                                 [id](const Charger& c) { return c.id == id; });
    if (it == chargers_.cend())
    {
        notify(QStringLiteral("请先选择电桩"), true);
        return;
    }
    const auto charger = *it;
    bool ok = false;
    const QStringList states{QStringLiteral("空闲"), QStringLiteral("故障"),
                             QStringLiteral("已停用")};
    const auto selected = QInputDialog::getItem(this, QStringLiteral("修改电桩状态"),
                                                QStringLiteral("目标状态"), states, 0, false, &ok);
    if (!ok || !api_.hasSession())
        return;
    const int target = selected == states[0] ? 0 : selected == states[1] ? 2 : 3;
    QString reason;
    if (!confirmReason(QStringLiteral("确认状态变更"),
                       QStringLiteral("将 %1 的状态改为「%2」").arg(charger.code, selected),
                       &reason))
        return;
    mutate("PUT", QStringLiteral("admin/chargers/%1/status").arg(charger.id),
           {{"targetStatus", target}, {"version", charger.version}, {"reason", reason}}, 2);
}
void AdminMainWindow::toggleUserStatus()
{
    if (operationBusy_)
        return;
    const auto id = selectedId(userTable_);
    if (id <= 0)
    {
        notify(QStringLiteral("请先选择用户"), true);
        return;
    }
    setBusy(true);
    api_.get(QStringLiteral("admin/users/%1").arg(id), {},
             [this, id](AdminReply reply)
             {
                 if (!reply.ok())
                 {
                     setBusy(false);
                     notify(reply.message, true);
                     return;
                 }
                 const auto data = reply.data.toObject();
                 const qint64 version = data.value("version").toInteger();
                 const int status = data.value("status").toInt(-1);
                 if (version <= 0 || (status != 0 && status != 1))
                 {
                     setBusy(false);
                     notify(QStringLiteral("用户详情不完整，请刷新后重试"), true);
                     return;
                 }
                 const int target = status == 0 ? 1 : 0;
                 QString reason;
                 const QString title =
                     target == 0 ? QStringLiteral("冻结用户") : QStringLiteral("解冻用户");
                 QString description = QStringLiteral("确认%1 #%2？").arg(title).arg(id);
                 if (data.value("hasActiveFlow").toBool())
                     description +=
                         QStringLiteral("\n该用户有进行中的订单，冻结后仍保留已有订单。");
                 const bool confirmed = confirmReason(title, description, &reason);
                 setBusy(false);
                 if (!confirmed)
                     return;
                 mutate("PUT", QStringLiteral("admin/users/%1/status").arg(id),
                        {{"status", target}, {"version", version}, {"reason", reason}}, 3);
             });
}
void AdminMainWindow::restartCharger()
{
    if (operationBusy_)
        return;
    const auto id = selectedId(chargerTable_);
    const auto it = std::find_if(chargers_.cbegin(), chargers_.cend(),
                                 [id](const Charger& c) { return c.id == id; });
    if (it == chargers_.cend())
    {
        notify(QStringLiteral("请先选择电桩"), true);
        return;
    }
    const auto charger = *it;
    QString reason;
    if (!confirmReason(QStringLiteral("远程重启"),
                       QStringLiteral("重启 %1 将暂时中断设备服务，确认继续？").arg(charger.code),
                       &reason))
        return;
    reauthenticate(
        [this, charger, reason]
        {
            setBusy(true);
            api_.postJson(
                QStringLiteral("admin/chargers/%1/restart-commands").arg(charger.id),
                {{"confirm", true}, {"reason", reason}},
                [this](AdminReply reply)
                {
                    if (!reply.ok())
                    {
                        setBusy(false);
                        if (reply.httpStatus == 403)
                            reauthExpiresAt_ = 0;
                        notify(reply.message, true);
                        return;
                    }
                    const auto command = reply.data.toObject().value("commandNo").toString();
                    if (command.isEmpty())
                    {
                        setBusy(false);
                        notify(QStringLiteral("重启已提交，但未返回指令编号；请刷新查看设备状态"),
                               true);
                        return;
                    }
                    notify(QStringLiteral("重启指令已提交，正在等待设备响应…"));
                    pollCommand(command, 0);
                },
                true);
        });
}
void AdminMainWindow::pollCommand(const QString& commandNo, int attempts)
{
    api_.get(
        QStringLiteral("admin/device-commands/%1").arg(commandNo), {},
        [this, commandNo, attempts](AdminReply reply)
        {
            if (!reply.ok())
            {
                setBusy(false);
                notify(QStringLiteral("指令已提交，状态查询失败：%1").arg(reply.message), true);
                return;
            }
            const auto status = reply.data.toObject().value("status").toString();
            if (status == "SUCCEEDED" || status == "FAILED" || status == "TIMED_OUT")
            {
                setBusy(false);
                notify(status == "SUCCEEDED" ? QStringLiteral("设备重启成功")
                                             : QStringLiteral("设备重启未成功，请检查设备后重试"),
                       status != "SUCCEEDED");
                refreshChargers();
                return;
            }
            if (attempts >= 30)
            {
                setBusy(false);
                notify(QStringLiteral("指令 %1 仍在处理中，请稍后刷新设备状态").arg(commandNo));
                refreshChargers();
                return;
            }
            const auto session = sessionGeneration_;
            QTimer::singleShot(1000, this,
                               [this, commandNo, attempts, session]
                               {
                                   if (session == sessionGeneration_ && api_.hasSession())
                                       pollCommand(commandNo, attempts + 1);
                               });
        });
}
void AdminMainWindow::runPrediction()
{
    if (operationBusy_)
        return;
    setBusy(true);
    api_.postJson(
        "admin/ml-tasks", {{"taskType", "PREDICT"}, {"horizonHours", QJsonArray{1, 6, 24}}},
        [this](AdminReply reply)
        {
            if (!reply.ok())
            {
                setBusy(false);
                notify(reply.message, true);
                return;
            }
            const auto task = reply.data.toObject().value("taskNo").toString();
            if (task.isEmpty())
            {
                setBusy(false);
                notify(QStringLiteral("预测请求已提交，但未返回任务编号；请稍后查询结果"), true);
                return;
            }
            notify(QStringLiteral("预测任务已提交，正在生成结果…"));
            pollPrediction(task, 0);
        },
        true);
}
void AdminMainWindow::pollPrediction(const QString& taskNo, int attempts)
{
    api_.get(QStringLiteral("admin/ml-tasks/%1").arg(taskNo), {},
             [this, taskNo, attempts](AdminReply reply)
             {
                 if (!reply.ok())
                 {
                     setBusy(false);
                     notify(QStringLiteral("任务已提交，状态查询失败：%1").arg(reply.message),
                            true);
                     return;
                 }
                 const auto status = reply.data.toObject().value("status").toString();
                 if (status == "SUCCEEDED")
                 {
                     setBusy(false);
                     notify(QStringLiteral("预测已完成，正在更新结果"));
                     refreshPredictions();
                     return;
                 }
                 if (status == "FAILED" || status == "TIMED_OUT")
                 {
                     setBusy(false);
                     notify(QStringLiteral("预测任务未成功，请检查服务后重试"), true);
                     return;
                 }
                 if (attempts >= 30)
                 {
                     setBusy(false);
                     notify(QStringLiteral("任务 %1 仍在运行，请稍后查询结果").arg(taskNo));
                     return;
                 }
                 const auto session = sessionGeneration_;
                 QTimer::singleShot(1000, this,
                                    [this, taskNo, attempts, session]
                                    {
                                        if (session == sessionGeneration_ && api_.hasSession())
                                            pollPrediction(taskNo, attempts + 1);
                                    });
             });
}
} // namespace ncs::admin
