#include "user_main_window.h"

#include "ui/charger_table.h"
#include "ui/charge_soc_gauge.h"
#include "ui/bottom_navigation.h"

#include <QLabel>
#include <QLineEdit>
#include <QKeyEvent>
#include <QPixmap>
#include <QStackedWidget>
#include <QTimer>

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
    const StationSummary station = service_.stations().at(stationId - 1);
    detailTitle_->setText(station.name);
    detailMeta_->setText(station.address + QStringLiteral("  ·  ") + station.distance +
                         QStringLiteral("  ·  ") + money(station.priceCentPerKwh) + QStringLiteral(" / 度"));
    chargerTable_->setChargers(service_.chargers(stationId));
    pages_->setCurrentIndex(kDetailPage);
}

void UserMainWindow::showCharge()
{
    bottomNavigation_->hide();
    reservationCountdown_->setVisible(!chargingStarted_);
    pages_->setCurrentIndex(kChargePage);
}

void UserMainWindow::refreshCharge()
{
    if (!chargeDuration_) return;
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
    notice_->setText(message);
    notice_->setVisible(true);
    notice_->setStyleSheet(
        QStringLiteral("padding:7px 10px;border-radius:8px;font-size:12px;color:%1;background:%2;")
            .arg(error ? QStringLiteral("#B42318") : QStringLiteral("#0F766E"),
                 error ? QStringLiteral("#FFF0F0") : QStringLiteral("#E2F3F0")));
    const int timeoutMs = error ? 5000 : 3200;
    QTimer::singleShot(timeoutMs, notice_, [notice = notice_, message] {
        if (notice->text() == message) notice->hide();
    });
}

} // namespace ncs::user
