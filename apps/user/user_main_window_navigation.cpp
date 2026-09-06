#include "user_main_window.h"

#include "net/user_api.h"
#include "ui/bottom_navigation.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QToolButton>
#include <QToolTip>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QtMath>

#ifdef NCS_HAS_WEBENGINE
#include <QWebEngineView>
#endif

namespace ncs::user
{
namespace
{
NavigationRoute browserFallbackRoute(const StationSummary& station, const QString& mode,
                                     const QString& distance)
{
    const QString routeType = mode == QStringLiteral("walking")   ? QStringLiteral("walk")
                              : mode == QStringLiteral("transit") ? QStringLiteral("bus")
                                                                  : QStringLiteral("drive");
    QUrl url(QStringLiteral("https://apis.map.qq.com/uri/v1/routeplan"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("type"), routeType);
    query.addQueryItem(QStringLiteral("from"), QStringLiteral("当前位置"));
    query.addQueryItem(QStringLiteral("to"),
                       QStringLiteral("%1,%2,%3")
                           .arg(QString::number(station.latitude, 'f', 6),
                                QString::number(station.longitude, 'f', 6), station.name));
    url.setQuery(query);
    return {station.name, station.address, distance.isEmpty() ? station.distance : distance, mode,
            url.toString()};
}
} // namespace

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
    routeHelp->setToolTip(
        QStringLiteral("路线会在系统浏览器中打开；地图服务暂不可用时仍可使用该方式继续导航。"));
    routeHelp->setStyleSheet(QStringLiteral(
        "QToolButton{color:#0F766E;border:0;background:transparent;font-size:17px;padding:2px;}"));
    heading->addWidget(routeHelp);
    heading->addStretch();
    layout->addLayout(heading);
    layout->addWidget(new QLabel(QStringLiteral("起点：当前位置")));
    navigationMode_ = new QComboBox;
    navigationMode_->addItem(QStringLiteral("驾车"), QStringLiteral("driving"));
    navigationMode_->addItem(QStringLiteral("步行"), QStringLiteral("walking"));
    navigationMode_->addItem(QStringLiteral("公交"), QStringLiteral("transit"));
    layout->addWidget(navigationMode_);
#ifdef NCS_HAS_WEBENGINE
    auto* map = new QWebEngineView;
    map->setMinimumHeight(270);
    map->setVisible(false);
    navigationMap_ = map;
    layout->addWidget(map, 1);
#endif
    navigationSummary_ = new QLabel;
    navigationSummary_->setWordWrap(true);
    navigationSummary_->setStyleSheet(
        QStringLiteral("padding:24px;background:#FFFFFF;border:1px solid "
                       "#E4EAF3;border-radius:16px;font-size:15px;"));
    layout->addWidget(navigationSummary_);
    navigationBrowserButton_ = button(QStringLiteral("地图不可用时在浏览器继续导航"));
    auto* back = button(QStringLiteral("返回电站详情"),
                        QStringLiteral("QPushButton{background:#E2F3F0;color:#0F766E;border:0;"
                                       "border-radius:10px;font-size:15px;font-weight:600;}"));
    layout->addWidget(navigationBrowserButton_);
    layout->addWidget(back);
    layout->addStretch();
    connect(navigationMode_, &QComboBox::currentIndexChanged, this, [this] { showNavigation(); });
    connect(navigationBrowserButton_, &QPushButton::clicked, this,
            [this]
            {
                const QUrl target(navigationRoute_.url);
                if (!target.isValid() || target.scheme() != QStringLiteral("https"))
                {
                    notify(QStringLiteral("导航链接无效"), true);
                    return;
                }
                QDesktopServices::openUrl(target);
            });
    connect(back, &QPushButton::clicked, this, [this] { showDetail(selectedStationId_); });
    connect(routeHelp, &QToolButton::clicked, this,
            [routeHelp]
            {
                QToolTip::showText(routeHelp->mapToGlobal(QPoint(0, routeHelp->height())),
                                   routeHelp->toolTip(), routeHelp);
            });
    return page;
}

void UserMainWindow::showNavigation()
{
    bottomNavigation_->hide();
    const QString mode = navigationMode_->currentData().toString();
    const int requestId = ++navigationRequestId_;
    if (userApi_)
    {
        const auto station = stationsById_.constFind(selectedStationId_);
        if (station == stationsById_.cend())
        {
            notify(QStringLiteral("站点信息已更新，请重新选择"), true);
            showHome();
            return;
        }
        navigationRoute_ = browserFallbackRoute(*station, mode, selectedStationDistance_);
    }
    else
        navigationRoute_ = service_.route(selectedStationId_, mode);
    navigationSummary_->setText(QStringLiteral("正在向腾讯地图请求路线…"));
    navigationBrowserButton_->setEnabled(!navigationRoute_.url.isEmpty());
#ifdef NCS_HAS_WEBENGINE
    navigationMap_->setVisible(false);
#endif
    pages_->setCurrentIndex(7);
    if (!onlineSession_ || !userApi_)
    {
        showNavigationFallback(QStringLiteral("服务端会话不可用"));
        return;
    }
    const int stationId = selectedStationId_;
    userApi_->navigationRoute(
        stationId, qRound64(stationsById_.value(stationId).latitude * 1000000),
        qRound64(stationsById_.value(stationId).longitude * 1000000),
        stationsById_.value(stationId).address, mode,
        [this, stationId, requestId](ApiReply reply)
        {
            if (requestId != navigationRequestId_ || stationId != selectedStationId_ ||
                pages_->currentIndex() != 7)
                return;
            if (!reply.ok() || !reply.data.isObject())
            {
                showNavigationFallback(reply.message);
                return;
            }
            applyNavigationRoute(reply.data.toObject());
        });
}

void UserMainWindow::showNavigationFallback(const QString& reason)
{
    const QString mode = navigationMode_->currentData().toString();
    if (userApi_)
    {
        const auto station = stationsById_.constFind(selectedStationId_);
        if (station == stationsById_.cend())
        {
            showHome();
            return;
        }
        navigationRoute_ = browserFallbackRoute(*station, mode, selectedStationDistance_);
    }
    else
        navigationRoute_ = service_.route(selectedStationId_, mode);
    const QString modeText = mode == QStringLiteral("walking")   ? QStringLiteral("步行")
                             : mode == QStringLiteral("transit") ? QStringLiteral("公交")
                                                                 : QStringLiteral("驾车");
    navigationSummary_->setText(
        QStringLiteral("腾讯路线暂不可用，已启用最终降级方案。\n终点：%1\n地址：%2\n直线距离：%"
                       "3\n出行方式：%4%5")
            .arg(navigationRoute_.stationName, navigationRoute_.destinationAddress,
                 navigationRoute_.distance, modeText,
                 reason.isEmpty() ? QString() : QStringLiteral("\n原因：") + reason));
    navigationBrowserButton_->setEnabled(!navigationRoute_.url.isEmpty());
#ifdef NCS_HAS_WEBENGINE
    navigationMap_->setVisible(false);
#endif
}

void UserMainWindow::applyNavigationRoute(const QJsonObject& routeData)
{
    navigationRoute_.stationName = routeData.value(QStringLiteral("stationName")).toString();
    navigationRoute_.destinationAddress =
        routeData.value(QStringLiteral("destinationAddress")).toString();
    navigationRoute_.mode = routeData.value(QStringLiteral("mode")).toString();
    navigationRoute_.url = routeData.value(QStringLiteral("browserUrl")).toString();
    const qint64 distanceMeter =
        routeData.value(QStringLiteral("distanceMeter")).toVariant().toLongLong();
    const qint64 durationSecond =
        routeData.value(QStringLiteral("durationSecond")).toVariant().toLongLong();
    navigationRoute_.distance = QStringLiteral("%1 km").arg(distanceMeter / 1000.0, 0, 'f', 1);
    const QString modeText =
        navigationRoute_.mode == QStringLiteral("walking")   ? QStringLiteral("步行")
        : navigationRoute_.mode == QStringLiteral("transit") ? QStringLiteral("公交")
                                                             : QStringLiteral("驾车");
    const bool routeFallback = routeData.value(QStringLiteral("routeFallback")).toBool();
    const bool locationFallback = routeData.value(QStringLiteral("locationFallback")).toBool();
    QString summary =
        QStringLiteral(
            "路线来源：%1\n终点：%2\n地址：%3\n路线距离：%4\n预计时间：%5 分钟\n出行方式：%6")
            .arg(routeFallback ? QStringLiteral("本地最终降级") : QStringLiteral("腾讯地图"),
                 navigationRoute_.stationName, navigationRoute_.destinationAddress,
                 navigationRoute_.distance,
                 durationSecond > 0 ? QString::number((durationSecond + 59) / 60)
                                    : QStringLiteral("--"),
                 modeText);
    if (locationFallback)
        summary += QStringLiteral("\n定位说明：地址定位失败，已使用默认坐标");
    navigationSummary_->setText(summary);
    navigationBrowserButton_->setEnabled(!navigationRoute_.url.isEmpty());
    if (routeFallback)
    {
#ifdef NCS_HAS_WEBENGINE
        navigationMap_->setVisible(false);
#endif
        return;
    }
    renderNavigationMap(routeData);
}

void UserMainWindow::renderNavigationMap(const QJsonObject& routeData)
{
#ifdef NCS_HAS_WEBENGINE
    if (tencentMapJsKey_.trimmed().isEmpty() ||
        routeData.value(QStringLiteral("polyline")).toArray().size() < 2)
    {
        navigationMap_->setVisible(false);
        return;
    }
    const QByteArray routeJson =
        QJsonDocument(routeData.value(QStringLiteral("polyline")).toArray())
            .toJson(QJsonDocument::Compact);
    const QByteArray key = QUrl::toPercentEncoding(tencentMapJsKey_.trimmed());
    QString html = QString::fromUtf8(R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>html,body,#map{width:100%;height:100%;margin:0}#error{display:none;position:absolute;z-index:2;left:8px;right:8px;top:8px;padding:8px;background:#fee2e2;color:#991b1b;border-radius:6px}</style>
</head><body><div id="map"></div><div id="error"></div>
<script>function fail(){const e=document.getElementById('error');e.textContent='内嵌地图不可用，请使用浏览器导航';e.style.display='block'}</script>
<script src="https://map.qq.com/api/gljs?v=1&key=__KEY__" onerror="fail()"></script>
<script>
try {
 const route=__ROUTE__;
 const points=route.map(p=>new TMap.LatLng(p.latitudeE6/1e6,p.longitudeE6/1e6));
 const map=new TMap.Map(document.getElementById('map'),{center:points[0],zoom:12});
 new TMap.MultiPolyline({map:map,styles:{route:new TMap.PolylineStyle({color:'#0F766E',width:7,borderWidth:2,borderColor:'#ffffff',lineCap:'round'})},geometries:[{id:'route',styleId:'route',paths:points}]});
 new TMap.MultiMarker({map:map,geometries:[{id:'origin',position:points[0]},{id:'destination',position:points[points.length-1]}]});
 const bounds=new TMap.LatLngBounds(); points.forEach(p=>bounds.extend(p)); map.fitBounds(bounds,{padding:48});
} catch(e) { fail(); }
</script></body></html>)HTML");
    html.replace(QStringLiteral("__KEY__"), QString::fromLatin1(key));
    html.replace(QStringLiteral("__ROUTE__"), QString::fromUtf8(routeJson));
    auto* map = static_cast<QWebEngineView*>(navigationMap_);
    const QUrl origin(tencentMapJsOrigin_.isEmpty() ? QStringLiteral("http://localhost/")
                                                    : tencentMapJsOrigin_);
    map->setHtml(html, origin);
    map->setVisible(true);
#else
    Q_UNUSED(routeData)
#endif
}

} // namespace ncs::user
