#include "user_main_window.h"

#include "ui/bottom_navigation.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QToolButton>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>

namespace ncs::user
{

QWidget* UserMainWindow::createNavigationPage()
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 8, 0, 0);
    layout->setSpacing(12);
    auto* title = new QLabel(QStringLiteral("路线导航"));
    title->setStyleSheet(QStringLiteral("font-size:23px;color:#25324A;"));
    auto* heading = new QHBoxLayout;
    heading->addWidget(title);
    auto* routeHelp = new QToolButton;
    routeHelp->setText(QStringLiteral("ⓘ"));
    routeHelp->setToolTip(QStringLiteral("路线将在地图服务中打开。"));
    routeHelp->setStyleSheet(QStringLiteral("QToolButton{color:#0F766E;border:0;background:transparent;font-size:17px;padding:2px;}"));
    heading->addWidget(routeHelp);
    heading->addStretch();
    layout->addLayout(heading);
    layout->addWidget(new QLabel(QStringLiteral("起点：当前位置")));
    navigationMode_ = new QComboBox;
    navigationMode_->addItem(QStringLiteral("驾车"), QStringLiteral("driving"));
    navigationMode_->addItem(QStringLiteral("步行"), QStringLiteral("walking"));
    layout->addWidget(navigationMode_);
    navigationSummary_ = new QLabel;
    navigationSummary_->setWordWrap(true);
    navigationSummary_->setStyleSheet(QStringLiteral("padding:24px;background:#FFFFFF;border:1px solid #E4EAF3;border-radius:16px;font-size:15px;"));
    layout->addWidget(navigationSummary_);
    auto* open = button(QStringLiteral("打开地图路线"));
    auto* back = button(QStringLiteral("返回电站详情"), QStringLiteral("QPushButton{background:#E2F3F0;color:#0F766E;border:0;border-radius:10px;font-size:15px;font-weight:600;}"));
    layout->addWidget(open);
    layout->addWidget(back);
    layout->addStretch();
    connect(navigationMode_, &QComboBox::currentIndexChanged, this, [this] { showNavigation(); });
    connect(open, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl(navigationRoute_.url)); });
    connect(back, &QPushButton::clicked, this, [this] { showDetail(selectedStationId_); });
    connect(routeHelp, &QToolButton::clicked, this, [routeHelp] {
        QToolTip::showText(routeHelp->mapToGlobal(QPoint(0, routeHelp->height())), routeHelp->toolTip(), routeHelp);
    });
    return page;
}

void UserMainWindow::showNavigation()
{
    bottomNavigation_->hide();
    const QString mode = navigationMode_->currentData().toString();
    navigationRoute_ = service_.route(selectedStationId_, mode);
    navigationSummary_->setText(
        QStringLiteral("终点：%1\n地址：%2\n距离：%3\n出行方式：%4")
            .arg(navigationRoute_.stationName, navigationRoute_.destinationAddress,
                 navigationRoute_.distance, mode == QStringLiteral("walking") ? QStringLiteral("步行")
                                                               : QStringLiteral("驾车")));
    pages_->setCurrentIndex(7);
}

} // namespace ncs::user
