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
#include <QUrlQuery>
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
    auto* origin = new QLabel(QStringLiteral("起点：当前位置"));
    origin->setStyleSheet(QStringLiteral("font-size:13px;color:#667085;"));
    layout->addWidget(origin);
    navigationMode_ = new QComboBox;
    navigationMode_->addItem(QStringLiteral("驾车"), QStringLiteral("driving"));
    navigationMode_->addItem(QStringLiteral("步行"), QStringLiteral("walking"));
    navigationMode_->addItem(QStringLiteral("公交"), QStringLiteral("transit"));
    layout->addWidget(navigationMode_);
    auto* routeCard = new QFrame;
    routeCard->setObjectName(QStringLiteral("card"));
    auto* routeLayout = new QVBoxLayout(routeCard);
    routeLayout->setContentsMargins(18, 16, 18, 16);
    navigationSummary_ = new QLabel;
    navigationSummary_->setWordWrap(true);
    navigationSummary_->setStyleSheet(QStringLiteral("font-size:14px;color:#475467;line-height:1.6;"));
    routeLayout->addWidget(navigationSummary_);
    layout->addWidget(routeCard);
    auto* open = button(QStringLiteral("打开地图路线"));
    auto* back = button(QStringLiteral("返回电站详情"));
    back->setObjectName(QStringLiteral("secondaryButton"));
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
    if (userApi_)
    {
        const auto station = stationsById_.constFind(selectedStationId_);
        if (station == stationsById_.cend())
        {
            notify(QStringLiteral("站点信息已更新，请重新选择"), true);
            showHome();
            return;
        }
        const QString routeType = mode == QStringLiteral("walking") ? QStringLiteral("walk")
                                  : mode == QStringLiteral("transit") ? QStringLiteral("bus")
                                                                        : QStringLiteral("drive");
        QUrl url(QStringLiteral("https://apis.map.qq.com/uri/v1/routeplan"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("type"), routeType);
        query.addQueryItem(QStringLiteral("from"), QStringLiteral("当前位置"));
        query.addQueryItem(QStringLiteral("to"), QStringLiteral("%1,%2,%3")
            .arg(QString::number(station->latitude, 'f', 6), QString::number(station->longitude, 'f', 6), station->name));
        url.setQuery(query);
        navigationRoute_ = {station->name, station->address,
                            selectedStationDistance_.isEmpty() ? station->distance : selectedStationDistance_,
                            mode, url.toString()};
    }
    else
    {
        navigationRoute_ = service_.route(selectedStationId_, mode);
        if (!selectedStationDistance_.isEmpty()) navigationRoute_.distance = selectedStationDistance_;
    }
    const QString modeText = mode == QStringLiteral("walking") ? QStringLiteral("步行")
                           : mode == QStringLiteral("transit") ? QStringLiteral("公交")
                                                                  : QStringLiteral("驾车");
    navigationSummary_->setText(
        QStringLiteral("终点：%1\n地址：%2\n距离：%3\n出行方式：%4")
            .arg(navigationRoute_.stationName, navigationRoute_.destinationAddress,
                 navigationRoute_.distance, modeText));
    pages_->setCurrentIndex(7);
}

} // namespace ncs::user
