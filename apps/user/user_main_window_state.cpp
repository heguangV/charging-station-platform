#include "user_main_window.h"

#include "ui/charger_table.h"
#include "ui/charge_soc_gauge.h"
#include "ui/bottom_navigation.h"
#include "net/user_api.h"

#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QGraphicsOpacityEffect>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QDateTime>
#include <QPauseAnimation>
#include <QPixmap>
#include <QSequentialAnimationGroup>
#include <QStackedWidget>

namespace ncs::user
{
namespace
{
constexpr int kHomePage = 1;
constexpr int kDetailPage = 2;
constexpr int kChargePage = 3;
} // namespace

void UserMainWindow::showLogin()
{
    onlineSession_ = false;
    phoneEdit_->clear();
    codeEdit_->clear();
    bottomNavigation_->hide();
    pages_->setCurrentIndex(0);
}

void UserMainWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->key() != Qt::Key_Escape)
    {
        QMainWindow::keyPressEvent(event);
        return;
    }
    switch (pages_->currentIndex())
    {
    case 2:
        showHome();
        break;
    case 3:
        if (chargingStarted_)
            notify(QStringLiteral("充电进行中，请先结束充电并结算"), true);
        else
            showDetail(selectedStationId_);
        break;
    case 4:
        showHome();
        break;
    case 5:
        showHome();
        break;
    case 6:
        showProfile();
        break;
    case 7:
        showDetail(selectedStationId_);
        break;
    default:
        QMainWindow::keyPressEvent(event);
        return;
    }
    event->accept();
}

void UserMainWindow::showHome()
{
    bottomNavigation_->setCurrent(BottomNavigation::Item::Home);
    bottomNavigation_->show();
    pages_->setCurrentIndex(kHomePage);
}

void UserMainWindow::showProfile()
{
    refreshProfile();
    bottomNavigation_->setCurrent(BottomNavigation::Item::Profile);
    bottomNavigation_->show();
    pages_->setCurrentIndex(5);
}

void UserMainWindow::showDetail(int stationId)
{
    bottomNavigation_->hide();
    selectedStationId_ = stationId;
    if (userApi_)
    {
        const auto station = stationsById_.constFind(stationId);
        if (station == stationsById_.cend())
        {
            notify(QStringLiteral("站点信息已更新，请重新选择"), true);
            showHome();
            return;
        }
        detailTitle_->setText(station->name);
        const QString distance = selectedStationDistance_.isEmpty() ? station->distance : selectedStationDistance_;
        detailMeta_->setText(station->address + QStringLiteral("\n") + distance + QStringLiteral(" · ") +
                             money(station->priceCentPerKwh) + QStringLiteral(" / 度"));
        chargerTable_->setChargers({});
        pages_->setCurrentIndex(kDetailPage);
        userApi_->chargers(stationId, [this, stationId](ApiReply reply) {
            if (stationId != selectedStationId_) return;
            if (!reply.ok())
            {
                notify(reply.message, true);
                return;
            }
            QVector<ChargerSummary> chargers;
            for (const QJsonValue& value : reply.data.toObject().value(QStringLiteral("items")).toArray())
            {
                const QJsonObject item = value.toObject();
                ChargerSummary charger;
                charger.id = item.value(QStringLiteral("id")).toVariant().toLongLong();
                charger.code = item.value(QStringLiteral("code")).toString();
                charger.type = item.value(QStringLiteral("chargerTypeText")).toString();
                charger.typeValue = item.value(QStringLiteral("chargerType")).toInt();
                charger.powerKw = item.value(QStringLiteral("powerWatt")).toInt() / 1000;
                charger.status = item.value(QStringLiteral("statusText")).toString();
                charger.totalCount = item.value(QStringLiteral("totalCount")).toInt();
                if (!charger.code.isEmpty()) chargers.append(std::move(charger));
            }
            chargerTable_->setChargers(chargers);
        });
        return;
    }
    const StationSummary station = service_.stations().at(stationId - 1);
    detailTitle_->setText(station.name);
    const QString distance = selectedStationDistance_.isEmpty() ? station.distance : selectedStationDistance_;
    detailMeta_->setText(station.address + QStringLiteral("\n") + distance + QStringLiteral(" · ") +
                         money(station.priceCentPerKwh) + QStringLiteral(" / 度"));
    chargerTable_->setChargers(service_.chargers(stationId));
    pages_->setCurrentIndex(kDetailPage);
}

void UserMainWindow::showCharge()
{
    bottomNavigation_->hide();
    reservationCountdown_->setVisible(!chargingStarted_);
    pages_->setCurrentIndex(kChargePage);
}

void UserMainWindow::restoreActiveFlow()
{
    if (!userApi_)
    {
        showHome();
        return;
    }
    userApi_->activeFlow([this](ApiReply reply) {
        if (!reply.ok())
        {
            notify(reply.message, true);
            showHome();
            return;
        }
        const QJsonObject data = reply.data.toObject();
        if (!data.value(QStringLiteral("hasActiveFlow")).toBool())
        {
            showHome();
            return;
        }
        restoreFlow(data.value(QStringLiteral("flow")).toObject());
    });
}

void UserMainWindow::beginFlowRequest()
{
    selectedChargerCode_ = chargerTable_->selectedChargerCode();
    selectedChargerId_ = chargerTable_->selectedChargerId();
    selectedChargerType_ = chargerTable_->selectedChargerType();
    if (selectedChargerCode_.isEmpty() || selectedChargerId_ <= 0)
    {
        notify(QStringLiteral("请先选择空闲电桩"), true);
        return;
    }
    userApi_->activeFlow([this](ApiReply active) {
        if (!active.ok())
        {
            notify(active.message, true);
            return;
        }
        const QJsonObject activeData = active.data.toObject();
        if (activeData.value(QStringLiteral("hasActiveFlow")).toBool())
        {
            notify(QStringLiteral("您有未完成的充电订单，请先处理"), true);
            restoreFlow(activeData.value(QStringLiteral("flow")).toObject());
            return;
        }
        userApi_->requestFlow(selectedStationId_, selectedChargerType_, selectedChargerId_,
                              [this](ApiReply reply) {
            if (!reply.ok())
            {
                notify(reply.message, true);
                return;
            }
            restoreFlow(reply.data.toObject());
        });
    });
}

void UserMainWindow::restoreFlow(const QJsonObject& flow)
{
    activeFlowNo_ = flow.value(QStringLiteral("flowNo")).toString();
    activeFlowVersion_ = flow.value(QStringLiteral("version")).toVariant().toLongLong();
    activeFlowStatus_ = flow.value(QStringLiteral("status")).toInt();
    selectedStationId_ = flow.value(QStringLiteral("stationId")).toInt(selectedStationId_);
    selectedChargerCode_ = flow.value(QStringLiteral("chargerCode")).toString(selectedChargerCode_);
    reservationUntil_ = flow.value(QStringLiteral("reservedUntil")).toVariant().toLongLong();
    flowPollTicks_ = 0;

    if (activeFlowNo_.isEmpty())
    {
        notify(QStringLiteral("活动流程数据不完整"), true);
        showHome();
        return;
    }
    if (activeFlowStatus_ == 10)
    {
        chargingStarted_ = false;
        chargeState_->setText(flow.value(QStringLiteral("statusText")).toString());
        reservationCountdown_->setText(QStringLiteral("正在等待空闲电桩，可取消排队"));
        startButton_->setEnabled(false);
        cancelButton_->setEnabled(true);
        settleButton_->setEnabled(false);
        showCharge();
        return;
    }
    if (activeFlowStatus_ == 20)
    {
        const QJsonObject quote = flow.value(QStringLiteral("quote")).toObject();
        if (quote.isEmpty())
        {
            notify(QStringLiteral("报价已失效，正在同步流程"), true);
            return;
        }
        const QString quoteNo = quote.value(QStringLiteral("quoteNo")).toString();
        const int price = quote.value(QStringLiteral("totalPriceCentPerKwh")).toInt();
        if (QMessageBox::question(this, QStringLiteral("确认充电报价"),
                                  QStringLiteral("电桩：%1\n总价：%2 / 度\n报价有效期至：%3\n\n确认预约吗？")
                                      .arg(quote.value(QStringLiteral("chargerCode")).toString(), money(price),
                                           QDateTime::fromSecsSinceEpoch(quote.value(QStringLiteral("expiresAt")).toVariant().toLongLong(), Qt::UTC)
                                               .toLocalTime().toString(QStringLiteral("HH:mm:ss")))) != QMessageBox::Yes)
        {
            userApi_->cancelFlow(activeFlowNo_, activeFlowVersion_, QStringLiteral("USER_DECLINED"),
                                 [this](ApiReply reply) {
                if (!reply.ok()) notify(reply.message, true);
                activeFlowNo_.clear();
                showHome();
            });
            return;
        }
        userApi_->confirmQuote(activeFlowNo_, quoteNo, activeFlowVersion_, [this](ApiReply confirmed) {
            if (!confirmed.ok())
            {
                notify(confirmed.message, true);
                return;
            }
            const QJsonObject value = confirmed.data.toObject();
            activeFlowNo_ = value.value(QStringLiteral("flowNo")).toString();
            activeFlowVersion_ = value.value(QStringLiteral("version")).toVariant().toLongLong();
            activeFlowStatus_ = value.value(QStringLiteral("status")).toInt();
            reservationUntil_ = value.value(QStringLiteral("reservedUntil")).toVariant().toLongLong();
            selectedChargerCode_ = value.value(QStringLiteral("chargerCode")).toString();
            chargingStarted_ = false;
            chargeState_->setText(QStringLiteral("已预约 · %1").arg(selectedChargerCode_));
            startButton_->setEnabled(true);
            cancelButton_->setEnabled(true);
            settleButton_->setEnabled(false);
            notify(QStringLiteral("报价已确认，请在保留时间内开始充电"));
            showCharge();
        });
        return;
    }
    if (activeFlowStatus_ == 30)
    {
        chargingStarted_ = false;
        chargeState_->setText(QStringLiteral("已预约 · %1").arg(selectedChargerCode_));
        startButton_->setEnabled(true);
        cancelButton_->setEnabled(true);
        settleButton_->setEnabled(false);
        showCharge();
        refreshCharge();
        return;
    }
    if (activeFlowStatus_ == 40)
    {
        chargingStarted_ = true;
        chargeState_->setText(QStringLiteral("充电中 · %1").arg(selectedChargerCode_));
        startButton_->setEnabled(false);
        cancelButton_->setEnabled(false);
        settleButton_->setEnabled(true);
        showCharge();
        refreshCharge();
        return;
    }
    if (activeFlowStatus_ == 50 || activeFlowStatus_ == 80)
    {
        chargingStarted_ = false;
        chargeState_->setText(activeFlowStatus_ == 80 ? QStringLiteral("结算失败，请重试")
                                                       : QStringLiteral("结算处理中，请稍后重试"));
        startButton_->setEnabled(false);
        cancelButton_->setEnabled(false);
        settleButton_->setEnabled(true);
        showCharge();
        return;
    }
    notify(QStringLiteral("该充电流程已结束"));
    activeFlowNo_.clear();
    showHome();
}

void UserMainWindow::refreshCharge()
{
    if (!chargeDuration_) return;
    if (userApi_)
    {
        if (activeFlowNo_.isEmpty() || progressRequestInFlight_) return;
        if (!chargingStarted_)
        {
            if (activeFlowStatus_ == 10 || activeFlowStatus_ == 20 || activeFlowStatus_ == 30)
            {
                if (++flowPollTicks_ < 5) return;
                flowPollTicks_ = 0;
                progressRequestInFlight_ = true;
                userApi_->flow(activeFlowNo_, [this](ApiReply reply) {
                    progressRequestInFlight_ = false;
                    if (!reply.ok())
                    {
                        notify(reply.message, true);
                        return;
                    }
                    const QJsonObject flow = reply.data.toObject();
                    if (flow.value(QStringLiteral("status")).toInt() != activeFlowStatus_)
                    {
                        notify(QStringLiteral("充电流程状态已更新"));
                        restoreFlow(flow);
                    }
                });
                return;
            }
            const qint64 remaining = reservationUntil_ - QDateTime::currentSecsSinceEpoch();
            if (remaining > 0)
                reservationCountdown_->setText(QStringLiteral("请在 %1:%2 内开始充电")
                    .arg(remaining / 60, 2, 10, QLatin1Char('0'))
                    .arg(remaining % 60, 2, 10, QLatin1Char('0')));
            else if (reservationUntil_ > 0)
                reservationCountdown_->setText(QStringLiteral("预约可能已超时，正在同步状态"));
            return;
        }
        progressRequestInFlight_ = true;
        userApi_->progress(activeFlowNo_, [this](ApiReply reply) {
            progressRequestInFlight_ = false;
            if (!reply.ok())
            {
                notify(reply.message, true);
                return;
            }
            const QJsonObject value = reply.data.toObject();
            const qint64 duration = value.value(QStringLiteral("durationSec")).toVariant().toLongLong();
            chargeDuration_->setText(QStringLiteral("%1:%2:%3")
                .arg(duration / 3600, 2, 10, QLatin1Char('0'))
                .arg(duration / 60 % 60, 2, 10, QLatin1Char('0'))
                .arg(duration % 60, 2, 10, QLatin1Char('0')));
            chargeEnergy_->setText(QStringLiteral("%1 kWh").arg(QString::number(value.value(QStringLiteral("energyMwh")).toInteger() / 1000000.0, 'f', 3)));
            chargeAmount_->setText(money(value.value(QStringLiteral("amountCent")).toInt()));
            chargePower_->setText(QStringLiteral("%1 kW").arg(value.value(QStringLiteral("powerWatt")).toInt() / 1000));
            chargeSoc_->setValue(value.value(QStringLiteral("simulatedSoc")).toInt());
        });
        return;
    }
    const ChargeProgress value = service_.progress();
    const int hours = value.durationSeconds / 3600;
    const int minutes = value.durationSeconds / 60 % 60;
    const int seconds = value.durationSeconds % 60;
    chargeDuration_->setText(QStringLiteral("%1:%2:%3")
                                 .arg(hours, 2, 10, QLatin1Char('0'))
                                 .arg(minutes, 2, 10, QLatin1Char('0'))
                                 .arg(seconds, 2, 10, QLatin1Char('0')));
    chargeEnergy_->setText(
        QStringLiteral("%1 kWh").arg(QString::number(value.energyMwh / 1000000.0, 'f', 3)));
    chargeAmount_->setText(money(value.amountCent));
    chargePower_->setText(QStringLiteral("%1 kW").arg(value.powerKw));
    chargeSoc_->setValue(value.soc);
    if (!chargingStarted_ && !selectedChargerCode_.isEmpty())
    {
        const int seconds = service_.reservationRemainingSeconds();
        if (seconds > 0)
        {
            reservationCountdown_->setText(
                QStringLiteral("请在 %1:%2 内开始充电").arg(seconds / 60, 2, 10, QLatin1Char('0'))
                    .arg(seconds % 60, 2, 10, QLatin1Char('0')));
        }
        else
        {
            chargeState_->setText(QStringLiteral("预约已超时"));
            reservationCountdown_->setText(QStringLiteral("请返回详情重新选择空闲桩"));
        }
    }
}

void UserMainWindow::refreshProfile()
{
    if (userApi_)
    {
        profileName_->setText(QStringLiteral("加载中…"));
        userApi_->currentProfile([this](ApiReply reply) {
            if (!reply.ok())
            {
                notify(reply.message, true);
                return;
            }
            const QJsonObject user = reply.data.toObject();
            profileVersion_ = user.value(QStringLiteral("version")).toVariant().toLongLong();
            nicknameEdit_->setText(user.value(QStringLiteral("nickname")).toString());
            profileName_->setText(user.value(QStringLiteral("phoneMasked")).toString());
            profileBalance_->setText(money(user.value(QStringLiteral("balanceCent")).toInt()));
            profileAvatar_->setText(QStringLiteral("NCS"));
            profileAvatar_->setPixmap({});
            if (user.value(QStringLiteral("avatarUrl")).toString().isEmpty()) return;
            userApi_->avatarContent([this](QByteArray bytes, const QString&, int status) {
                if (status < 200 || status >= 300 || bytes.isEmpty()) return;
                QPixmap avatar;
                if (!avatar.loadFromData(bytes)) return;
                profileAvatar_->setText({});
                profileAvatar_->setPixmap(avatar.scaled(profileAvatar_->size(), Qt::KeepAspectRatioByExpanding,
                                                         Qt::SmoothTransformation));
            });
        });
        return;
    }
    nicknameEdit_->setText(service_.nickname());
    profileName_->setText(service_.phoneMasked());
    profileBalance_->setText(money(service_.balanceCent()));
    const QPixmap avatar(service_.avatarPath());
    if (avatar.isNull())
    {
        profileAvatar_->setText(QStringLiteral("NCS"));
        profileAvatar_->setPixmap({});
    }
    else
    {
        profileAvatar_->setText({});
        profileAvatar_->setPixmap(avatar.scaled(profileAvatar_->size(), Qt::KeepAspectRatioByExpanding,
                                                 Qt::SmoothTransformation));
    }
}

void UserMainWindow::notify(const QString& message, bool error)
{
    noticeAnimation_->stop();
    notice_->setText(message);
    notice_->setStyleSheet(
        QStringLiteral("padding:10px 14px;border-radius:14px;font-size:13px;font-weight:600;color:%1;background:%2;")
            .arg(error ? QStringLiteral("#B42318") : QStringLiteral("#0F766E"),
                 error ? QStringLiteral("#FFF4F2") : QStringLiteral("#E7F6F2")));
    notice_->setFixedHeight(qMax(44, notice_->sizeHint().height()));
    notice_->move((centralWidget()->width() - notice_->width()) / 2, 18);
    noticeOpacity_->setOpacity(0.0);
    notice_->show();
    notice_->raise();
    noticePause_->setDuration(error ? 4200 : 2400);
    noticeAnimation_->start();
}

} // namespace ncs::user
