#include "user_main_window.h"

#include "ui/station_card.h"
#include "ui/charge_soc_gauge.h"
#include "ui/bottom_navigation.h"
#include "ui/charger_table.h"
#include "ui/station_list_widget.h"

#include <QFormLayout>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QPauseAnimation>
#include <QPropertyAnimation>
#include <QSequentialAnimationGroup>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtMath>

namespace ncs::user
{
namespace
{
constexpr int kLoginPage = 0;
constexpr int kHomePage = 1;
constexpr int kDetailPage = 2;
constexpr int kChargePage = 3;
constexpr int kReceiptPage = 4;
constexpr int kProfilePage = 5;

QLabel* label(const QString& value, int size = 14)
{
    auto* result = new QLabel(value);
    result->setWordWrap(true);
    result->setStyleSheet(QStringLiteral("font-size:%1px;color:#25324A;").arg(size));
    return result;
}

QFrame* card()
{
    auto* result = new QFrame;
    result->setObjectName(QStringLiteral("card"));
    return result;
}

QFrame* metricCard(const QString& title, QLabel*& value, const QString& initialValue)
{
    auto* result = card();
    auto* layout = new QVBoxLayout(result);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(5);
    auto* caption = label(title, 12);
    caption->setStyleSheet(QStringLiteral("font-size:12px;color:#667085;"));
    value = label(initialValue, 20);
    value->setStyleSheet(QStringLiteral("font-size:20px;color:#1D2939;font-weight:700;"));
    layout->addWidget(caption);
    layout->addWidget(value);
    return result;
}
} // namespace

UserMainWindow::UserMainWindow(UserClientService& service, QWidget* parent)
    : QMainWindow(parent), service_(service)
{
    setWindowTitle(QStringLiteral("NCS 充电"));
    setFixedSize(420, 760);
    auto* root = new QWidget(this);
    auto* layout = new QVBoxLayout(root);
    layout->setContentsMargins(18, 14, 18, 14);
    layout->setSpacing(10);
    notice_ = label({}, 13);
    notice_->setParent(root);
    notice_->setFixedWidth(330);
    notice_->setAlignment(Qt::AlignCenter);
    notice_->setWordWrap(true);
    noticeOpacity_ = new QGraphicsOpacityEffect(notice_);
    noticeOpacity_->setOpacity(0.0);
    notice_->setGraphicsEffect(noticeOpacity_);
    notice_->hide();
    pages_ = new QStackedWidget(root);
    pages_->addWidget(createLoginPage());
    pages_->addWidget(createHomePage());
    pages_->addWidget(createDetailPage());
    pages_->addWidget(createChargePage());
    pages_->addWidget(createReceiptPage());
    pages_->addWidget(createProfilePage());
    pages_->addWidget(createOrdersPage());
    pages_->addWidget(createNavigationPage());
    layout->addWidget(pages_);
    bottomNavigation_ = new BottomNavigation;
    bottomNavigation_->hide();
    layout->addWidget(bottomNavigation_);
    setCentralWidget(root);
    noticeAnimation_ = new QSequentialAnimationGroup(this);
    auto* fadeIn = new QPropertyAnimation(noticeOpacity_, "opacity", noticeAnimation_);
    fadeIn->setDuration(180);
    fadeIn->setStartValue(0.0);
    fadeIn->setEndValue(1.0);
    noticePause_ = new QPauseAnimation(noticeAnimation_);
    auto* fadeOut = new QPropertyAnimation(noticeOpacity_, "opacity", noticeAnimation_);
    fadeOut->setDuration(260);
    fadeOut->setStartValue(1.0);
    fadeOut->setEndValue(0.0);
    noticeAnimation_->addAnimation(fadeIn);
    noticeAnimation_->addAnimation(noticePause_);
    noticeAnimation_->addAnimation(fadeOut);
    connect(noticeAnimation_, &QSequentialAnimationGroup::finished, notice_, &QLabel::hide);
    connect(bottomNavigation_, &BottomNavigation::homeRequested, this, &UserMainWindow::showHome);
    connect(bottomNavigation_, &BottomNavigation::profileRequested, this, &UserMainWindow::showProfile);
    connect(bottomNavigation_, &BottomNavigation::ordersRequested, this, &UserMainWindow::showOrders);

    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, [this] {
        if (codeCountdown_ > 0)
        {
            --codeCountdown_;
            codeButton_->setText(QStringLiteral("%1 秒后重发").arg(codeCountdown_));
            if (codeCountdown_ == 0)
            {
                codeButton_->setEnabled(true);
                codeButton_->setText(QStringLiteral("获取验证码"));
            }
        }
        service_.tick();
        refreshCharge();
    });
    timer_->start(1000);
}

QPushButton* UserMainWindow::button(const QString& text, const QString& style)
{
    auto* result = new QPushButton(text);
    result->setCursor(Qt::PointingHandCursor);
    result->setMinimumHeight(44);
    if (style.isEmpty())
    {
        result->setObjectName(QStringLiteral("primaryButton"));
    }
    else
    {
        result->setStyleSheet(style);
    }
    return result;
}

QString UserMainWindow::money(int cent)
{
    return QStringLiteral("¥%1").arg(QString::number(cent / 100.0, 'f', 2));
}

QWidget* UserMainWindow::createHomePage()
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 8, 0, 0);
    auto* heading = new QHBoxLayout;
    heading->addWidget(label(QStringLiteral("附近充电站"), 23));
    auto* profile = button(QStringLiteral("我的"), QStringLiteral("QPushButton{color:#0F766E;border:0;background:transparent;font-size:14px;}"));
    heading->addStretch();
    heading->addWidget(profile);
    layout->addLayout(heading);
    auto* recommendation = new QFrame;
    recommendation->setStyleSheet(QStringLiteral("QFrame{background:#0F766E;border-radius:18px;}"));
    auto* recommendationLayout = new QVBoxLayout(recommendation);
    recommendationLayout->setContentsMargins(18, 16, 18, 16);
    auto* recommendationTitle = label(QStringLiteral("当前位置 · 中关村"), 13);
    recommendationTitle->setStyleSheet(QStringLiteral("color:#C9F4EC;font-size:13px;"));
    auto* recommendationText = label(QStringLiteral("附近有 10 个可用充电桩"), 21);
    recommendationText->setStyleSheet(QStringLiteral("color:white;font-size:21px;font-weight:700;"));
    auto* recommendationMeta = label(QStringLiteral("已为你优先排列距离近、空闲多的电站"), 12);
    recommendationMeta->setStyleSheet(QStringLiteral("color:#B9E8DF;font-size:12px;"));
    recommendationLayout->addWidget(recommendationTitle);
    recommendationLayout->addWidget(recommendationText);
    recommendationLayout->addWidget(recommendationMeta);
    layout->addWidget(recommendation);
    stationList_ = new StationListWidget(service_.stations());
    layout->addWidget(stationList_, 1);
    connect(stationList_, &StationListWidget::stationSelected, this, &UserMainWindow::showDetail);
    connect(profile, &QPushButton::clicked, this, &UserMainWindow::showProfile);
    return page;
}

QWidget* UserMainWindow::createDetailPage()
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 8, 0, 0);
    auto* back = button(QStringLiteral("‹ 返回附近电站"), QStringLiteral("QPushButton{color:#0F766E;border:0;background:transparent;text-align:left;font-size:14px;}"));
    layout->addWidget(back);
    detailTitle_ = label({}, 22);
    detailMeta_ = label({}, 13);
    layout->addWidget(detailTitle_);
    layout->addWidget(detailMeta_);
    chargerTable_ = new ChargerTable;
    layout->addWidget(chargerTable_);
    auto* navigate = button(QStringLiteral("一键导航"), QStringLiteral("QPushButton{background:#E2F3F0;color:#0F766E;border:0;border-radius:10px;font-size:15px;font-weight:600;}"));
    auto* reserve = button(QStringLiteral("选桩并预约"));
    layout->addWidget(navigate);
    layout->addWidget(reserve);
    layout->addStretch();
    connect(back, &QPushButton::clicked, this, &UserMainWindow::showHome);
    connect(navigate, &QPushButton::clicked, this, &UserMainWindow::showNavigation);
    connect(reserve, &QPushButton::clicked, this, [this] {
        if (service_.hasUnfinishedOrder())
        {
            QMessageBox::information(this, QStringLiteral("未完成订单"),
                                     QStringLiteral("您有未完成的充电订单，请先结算。"));
            showCharge();
            return;
        }
        QString message;
        selectedChargerCode_ = chargerTable_->selectedChargerCode();
        if (service_.reserve(selectedStationId_, selectedChargerCode_, &message))
        {
            chargingStarted_ = false;
            startButton_->setEnabled(true);
            cancelButton_->setEnabled(true);
            settleButton_->setEnabled(false);
            chargeState_->setText(QStringLiteral("已预约 · %1").arg(selectedChargerCode_));
            notify(message);
            showCharge();
        }
        else
        {
            notify(message, true);
        }
    });
    return page;
}

QWidget* UserMainWindow::createChargePage()
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 8, 0, 0);
    layout->setSpacing(14);
    layout->addWidget(label(QStringLiteral("充电控制"), 23));
    chargeState_ = label(QStringLiteral("已预约 · ZGC-DC-01"), 15);
    chargeState_->setStyleSheet(QStringLiteral("padding:10px 12px;background:#E8F8EF;color:#087443;border-radius:12px;font-size:15px;font-weight:600;"));
    layout->addWidget(chargeState_);
    reservationCountdown_ = label(QStringLiteral("预约保留中"), 13);
    reservationCountdown_->setStyleSheet(QStringLiteral("color:#B54708;font-size:13px;"));
    layout->addWidget(reservationCountdown_);
    auto* metrics = new QWidget;
    auto* grid = new QGridLayout(metrics);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(10);
    grid->addWidget(metricCard(QStringLiteral("充电时长"), chargeDuration_, QStringLiteral("00:00:00")), 0, 0);
    grid->addWidget(metricCard(QStringLiteral("累计电量"), chargeEnergy_, QStringLiteral("0.000 kWh")), 0, 1);
    grid->addWidget(metricCard(QStringLiteral("当前费用"), chargeAmount_, QStringLiteral("¥0.00")), 1, 0);
    grid->addWidget(metricCard(QStringLiteral("实时功率"), chargePower_, QStringLiteral("0 kW")), 1, 1);
    layout->addWidget(metrics);
    chargeSoc_ = new ChargeSocGauge;
    layout->addWidget(chargeSoc_);
    startButton_ = button(QStringLiteral("开始充电"));
    cancelButton_ = button(QStringLiteral("取消预约"), QStringLiteral("QPushButton{background:#FFF4E5;color:#B54708;border:0;border-radius:10px;font-size:15px;font-weight:600;}"));
    settleButton_ = button(QStringLiteral("结束充电并结算"), QStringLiteral("QPushButton{background:#0F9D71;color:white;border:0;border-radius:10px;font-size:15px;font-weight:600;}"));
    settleButton_->setEnabled(false);
    layout->addWidget(startButton_);
    layout->addWidget(cancelButton_);
    layout->addWidget(settleButton_);
    layout->addStretch();
    connect(startButton_, &QPushButton::clicked, this, [this] {
        QString message;
        if (service_.start(&message))
        {
            chargingStarted_ = true;
            chargeState_->setText(QStringLiteral("充电中 · %1 · 60 kW").arg(selectedChargerCode_));
            reservationCountdown_->hide();
            startButton_->setEnabled(false);
            cancelButton_->setEnabled(false);
            settleButton_->setEnabled(true);
            notify(message);
        }
        else
        {
            notify(message, true);
        }
    });
    connect(cancelButton_, &QPushButton::clicked, this, [this] {
        QString message;
        if (service_.cancelReservation(&message))
        {
            chargingStarted_ = false;
            selectedChargerCode_.clear();
            chargeState_->setText(QStringLiteral("预约已取消"));
            reservationCountdown_->setText(QStringLiteral("请返回详情重新选择空闲桩"));
            notify(message);
            showDetail(selectedStationId_);
        }
        else
        {
            notify(message, true);
        }
    });
    connect(settleButton_, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, QStringLiteral("确认结算"),
                                  QStringLiteral("结束充电后将立即从余额扣款，是否继续？")) !=
            QMessageBox::Yes)
        {
            return;
        }
        QString message;
        if (!service_.settle(&message))
        {
            notify(message, true);
            return;
        }
        const OrderSummary order = service_.orders().first();
        const int minutes = order.durationSeconds / 60;
        receiptText_->setText(
            QStringLiteral("支付成功\n\n订单号  %1\n电站  %2\n充电桩  %3\n开始时间  %4\n结束时间  %5\n\n充电时长  %6 分钟\n累计电量  %7 kWh\n单价  %8 / 度\n本次扣款  %9\n扣款后余额  %10\n\n感谢使用 NCS 充电服务")
                .arg(order.orderNo, order.stationName, order.chargerCode, order.startTime, order.endTime,
                     QString::number(minutes), QString::number(order.energyMwh / 1000000.0, 'f', 3),
                     money(service_.stations().at(selectedStationId_ - 1).priceCentPerKwh),
                     money(order.amountCent), money(service_.balanceCent())));
        chargingStarted_ = false;
        settleButton_->setEnabled(false);
        notify(message);
        pages_->setCurrentIndex(kReceiptPage);
    });
    return page;
}

QWidget* UserMainWindow::createReceiptPage()
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(14, 55, 14, 20);
    layout->addWidget(label(QStringLiteral("充电小票"), 25));
    receiptText_ = label({}, 16);
    receiptText_->setStyleSheet(QStringLiteral("padding:22px;background:white;border:1px solid #E9EDF5;border-radius:14px;font-size:16px;"));
    layout->addWidget(receiptText_);
    layout->addStretch();
    auto* done = button(QStringLiteral("完成，返回首页"));
    layout->addWidget(done);
    connect(done, &QPushButton::clicked, this, &UserMainWindow::showHome);
    return page;
}

} // namespace ncs::user
