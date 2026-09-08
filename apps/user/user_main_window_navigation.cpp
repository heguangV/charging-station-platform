#include "net/user_api.h"
#include "ui/bottom_navigation.h"
#include "ui/navigation_origin.h"
#include "ui/station_list_widget.h"
#include "user_main_window.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStringList>
#include <QTimer>
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
                                     const QString& originText)
{
    const auto origin = navigationOriginForText(originText);
    QUrl url(QStringLiteral("https://apis.map.qq.com/uri/v1/routeplan"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("type"),
                       mode == QStringLiteral("walking")   ? QStringLiteral("walk")
                       : mode == QStringLiteral("transit") ? QStringLiteral("bus")
                                                           : QStringLiteral("drive"));
    query.addQueryItem(QStringLiteral("from"), originText == QStringLiteral("当前位置（自动定位）")
                                                   ? QStringLiteral("当前位置")
                                                   : originText);
    if (origin.latitudeE6 && origin.longitudeE6)
        query.addQueryItem(QStringLiteral("fromcoord"),
                           QStringLiteral("%1,%2")
                               .arg(*origin.latitudeE6 / 1e6, 0, 'f', 6)
                               .arg(*origin.longitudeE6 / 1e6, 0, 'f', 6));
    query.addQueryItem(QStringLiteral("to"), station.name);
    const bool destinationKnown = station.latitude != 0.0 || station.longitude != 0.0;
    if (destinationKnown)
        query.addQueryItem(QStringLiteral("tocoord"), QStringLiteral("%1,%2")
                                                          .arg(station.latitude, 0, 'f', 6)
                                                          .arg(station.longitude, 0, 'f', 6));
    query.addQueryItem(QStringLiteral("referer"), QStringLiteral("NCS"));
    url.setQuery(query);
    QString distance = QStringLiteral("距离暂不可用");
    if (destinationKnown && origin.latitudeE6 && origin.longitudeE6)
    {
        const double lat = qDegreesToRadians(*origin.latitudeE6 / 1e6);
        const double dlat = qDegreesToRadians(station.latitude) - lat;
        const double dlon = qDegreesToRadians(station.longitude - *origin.longitudeE6 / 1e6);
        const double a = qPow(qSin(dlat / 2), 2) + qCos(lat) *
                                                       qCos(qDegreesToRadians(station.latitude)) *
                                                       qPow(qSin(dlon / 2), 2);
        distance = navigationDistance(qRound64(12742000.0 * qAsin(qSqrt(qBound(0.0, a, 1.0)))));
    }
    return {station.name, station.address, distance, mode, url.toString()};
}

QLabel* plainLabel(const QString& text, const QString& style)
{
    auto* label = new QLabel(text);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setStyleSheet(style);
    return label;
}
} // namespace

QWidget* UserMainWindow::createNavigationPage()
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(10);
    auto* heading = new QHBoxLayout;
    auto* back = button(QStringLiteral("‹"));
    back->setAccessibleName(QStringLiteral("返回电站详情"));
    back->setObjectName(QStringLiteral("secondaryButton"));
    back->setFixedWidth(40);
    heading->addWidget(back);
    heading->addWidget(plainLabel(QStringLiteral("到站导航"),
                                  QStringLiteral("font-size:24px;font-weight:800;color:#182B39;")),
                       1);
    layout->addLayout(heading);
    auto* locations = new QFrame;
    locations->setObjectName(QStringLiteral("card"));
    auto* locationsLayout = new QVBoxLayout(locations);
    locationsLayout->setContentsMargins(14, 12, 14, 12);
    locationsLayout->setSpacing(6);
    locationsLayout->addWidget(plainLabel(QStringLiteral("起点 · 自动定位 / 输入地址"),
                                          QStringLiteral("font-size:12px;color:#65717B;")));
    auto* originRow = new QHBoxLayout;
    navigationOrigin_ = new QComboBox;
    navigationOrigin_->setEditable(true);
    navigationOrigin_->setInsertPolicy(QComboBox::NoInsert);
    navigationOrigin_->setCompleter(nullptr);
    navigationOrigin_->setAccessibleName(QStringLiteral("导航起点"));
    navigationOrigin_->addItems(
        {QStringLiteral("当前位置（自动定位）"), QStringLiteral("中关村（模拟位置）"),
         QStringLiteral("望京（模拟位置）"), QStringLiteral("国贸（模拟位置）")});
    navigationOrigin_->lineEdit()->setMaxLength(160);
    navigationOrigin_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    navigationOrigin_->setMinimumWidth(0);
    navigationOrigin_->setStyleSheet(QStringLiteral(
        "QComboBox{background:#F5F7F9;border:1px solid #E5E9ED;"
        "border-radius:10px;padding:8px 20px 8px 8px;font-size:14px;color:#203442;}"
        "QComboBox::drop-down{border:0;width:22px;}"
        "QComboBox::down-arrow{image:url(:/ncs/resources/"
        "navigation-chevron.svg);width:12px;height:8px;}"
        "QComboBox QLineEdit{border:0;padding:0;background:transparent;font-size:14px;}"));
    originRow->addWidget(navigationOrigin_, 1);
    auto* locate = button(QStringLiteral("定位"));
    locate->setObjectName(QStringLiteral("secondaryButton"));
    locate->setAccessibleName(QStringLiteral("重新获取当前位置"));
    locate->setFixedWidth(64);
    originRow->addWidget(locate);
    locationsLayout->addLayout(originRow);
    navigationDestination_ =
        plainLabel({}, QStringLiteral("font-size:14px;font-weight:700;color:#1D3546;"));
    locationsLayout->addWidget(navigationDestination_);
    layout->addWidget(locations);
    auto* actions = new QHBoxLayout;
    navigationMode_ = new QComboBox;
    navigationMode_->setAccessibleName(QStringLiteral("出行方式"));
    navigationMode_->addItem(QStringLiteral("驾车"), QStringLiteral("driving"));
    navigationMode_->addItem(QStringLiteral("步行"), QStringLiteral("walking"));
    navigationMode_->addItem(QStringLiteral("公交"), QStringLiteral("transit"));
    navigationMode_->setStyleSheet(
        QStringLiteral("QComboBox{background:white;color:#203442;border:1px solid "
                       "#DFE6EA;border-radius:10px;padding:10px 24px 10px 14px;font-size:15px;}"
                       "QComboBox::drop-down{border:0;width:22px;}"
                       "QComboBox::down-arrow{image:url(:/ncs/resources/"
                       "navigation-chevron.svg);width:12px;height:8px;}"));
    actions->addWidget(navigationMode_, 1);
    navigationRetryButton_ = button(QStringLiteral("规划路线"));
    actions->addWidget(navigationRetryButton_, 1);
    layout->addLayout(actions);
    navigationSummary_ =
        plainLabel({}, QStringLiteral("font-size:14px;color:#244333;padding:4px 2px;"));
    layout->addWidget(navigationSummary_);
    navigationMapPanel_ = new QStackedWidget;
    navigationMapPanel_->setMinimumHeight(185);
    navigationMapMessage_ = plainLabel(
        {}, QStringLiteral("background:#FFFFFF;border:1px solid #E2E8EC;"
                           "border-radius:16px;padding:18px;font-size:14px;color:#596A77;"));
    navigationMapMessage_->setAlignment(Qt::AlignCenter);
    navigationMapPanel_->addWidget(navigationMapMessage_);
#ifdef NCS_HAS_WEBENGINE
    auto* map = new QWebEngineView;
    navigationMap_ = map;
    navigationMapPanel_->addWidget(map);
    connect(map, &QWebEngineView::titleChanged, this,
            [this](const QString& title)
            {
                if (pages_->currentIndex() != 7)
                    return;
                const auto suffix = QString::number(navigationRequestId_);
                if (title == QStringLiteral("NCS_MAP_READY_") + suffix)
                {
                    navigationMapReady_ = true;
                    navigationMapPanel_->setCurrentIndex(1);
                    navigationBrowserButton_->hide();
                }
                else if (title == QStringLiteral("NCS_MAP_ERROR_") + suffix)
                {
                    if (loadNavigationWebFallback())
                        return;
                    navigationMapPanel_->setCurrentIndex(0);
                    navigationMapMessage_->setText(
                        QStringLiteral("底图加载失败\n路线信息仍可查看，可重试或使用浏览器导航"));
                    navigationBrowserButton_->show();
                }
            });
#endif
    layout->addWidget(navigationMapPanel_, 1);
    navigationStepsArea_ = new QScrollArea;
    navigationStepsArea_->setWidgetResizable(true);
    navigationStepsArea_->setFrameShape(QFrame::NoFrame);
    navigationStepsArea_->setMaximumHeight(140);
    navigationStepsArea_->setVisible(false);
    navigationStepsText_ = new QLabel;
    navigationStepsText_->setTextFormat(Qt::RichText);
    navigationStepsText_->setWordWrap(true);
    navigationStepsText_->setStyleSheet(
        QStringLiteral("font-size:13px;color:#3D4B57;line-height:1.7;background:transparent;"));
    navigationStepsArea_->setWidget(navigationStepsText_);
    layout->addWidget(navigationStepsArea_);
    navigationBrowserButton_ = button(QStringLiteral("在腾讯地图中继续导航"));
    navigationBrowserButton_->setObjectName(QStringLiteral("secondaryButton"));
    navigationBrowserButton_->hide();
    layout->addWidget(navigationBrowserButton_);
    connect(navigationRetryButton_, &QPushButton::clicked, this, &UserMainWindow::showNavigation);
    connect(navigationMode_, &QComboBox::currentIndexChanged, this, [this] { showNavigation(); });
    connect(navigationOrigin_, &QComboBox::activated, this, [this] { showNavigation(); });
    connect(navigationOrigin_->lineEdit(), &QLineEdit::returnPressed, this,
            &UserMainWindow::showNavigation);
    connect(navigationOrigin_->lineEdit(), &QLineEdit::textEdited, this,
            [this]
            {
                ++navigationRequestId_;
                cancelNavigationLocation();
                navigationMapReady_ = false;
                navigationMapPanel_->setCurrentIndex(0);
                navigationMapMessage_->setText(QStringLiteral("起点已修改，点击规划路线"));
                navigationStepsArea_->setVisible(false);
                navigationSummary_->clear();
                navigationBrowserButton_->hide();
            });
    connect(locate, &QPushButton::clicked, this, &UserMainWindow::locateNavigationOrigin);
    connect(back, &QPushButton::clicked, this,
            [this]
            {
                ++navigationRequestId_;
                cancelNavigationLocation();
                showDetail(selectedStationId_);
            });
    connect(navigationBrowserButton_, &QPushButton::clicked, this,
            [this]
            {
                const QUrl target(navigationRoute_.url);
                if (!target.isValid() || target.scheme() != QStringLiteral("https") ||
                    target.host() != QStringLiteral("apis.map.qq.com"))
                {
                    notify(QStringLiteral("导航链接无效"), true);
                    return;
                }
                if (!QDesktopServices::openUrl(target))
                    navigationSummary_->setText(
                        QStringLiteral("无法打开系统浏览器，请重试；起终点信息已保留"));
            });
    return page;
}

void UserMainWindow::showNavigation()
{
    const bool entering = pages_->currentIndex() != 7;
    cancelNavigationLocation();
    bottomNavigation_->hide();
    const int requestId = ++navigationRequestId_;
    const QString mode = navigationMode_->currentData().toString();
    if (!userApi_)
    {
        for (const auto& item : service_.stations())
            stationsById_.insert(item.id, item);
    }
    const auto station = stationsById_.constFind(selectedStationId_);
    if (station == stationsById_.cend())
    {
        notify(QStringLiteral("请重新选择电站"), true);
        showHome();
        return;
    }
    navigationDestination_->setText(QStringLiteral("终点  %1").arg(station->name));
    navigationDestination_->setToolTip(station->address);
    pages_->setCurrentIndex(7);
    navigationMapReady_ = false;
    navigationMapPanel_->setCurrentIndex(0);
    navigationBrowserButton_->hide();
    navigationStepsArea_->setVisible(false);
    if (entering)
    {
        navigationHasDeviceCoordinate_ = false;
        locateNavigationOrigin();
        return;
    }
    navigationOriginText_ = navigationOrigin_->currentText().trimmed();
    auto origin = navigationOriginForText(navigationOriginText_);
    const bool gps = navigationOriginText_ == QStringLiteral("当前位置（自动定位）");
    if (gps)
    {
        if (!navigationHasDeviceCoordinate_)
        {
            locateNavigationOrigin();
            return;
        }
        origin.latitudeE6 = qRound64(navigationDeviceCoordinate_.y() * 1e6);
        origin.longitudeE6 = qRound64(navigationDeviceCoordinate_.x() * 1e6);
    }
    navigationRoute_ = browserFallbackRoute(*station, mode, navigationOriginText_);
    if (!origin.latitudeE6 && origin.keyword.isEmpty())
    {
        navigationSummary_->setText(QStringLiteral("请先选择位置或输入起点地址"));
        navigationMapMessage_->setText(QStringLiteral("起点确定后，这里将显示到站路线"));
        return;
    }
    navigationSummary_->setText(
        gps ? QStringLiteral("系统定位成功，正在规划路线…")
            : QStringLiteral("正在规划路线 · %1").arg(navigationOriginText_));
    navigationMapMessage_->setText(QStringLiteral("正在获取路线\n请稍候，也可修改起点后重新规划"));
    navigationRetryButton_->setText(QStringLiteral("重新规划"));
    if (!onlineSession_ || !userApi_)
    {
        showNavigationFallback(QStringLiteral("当前为演示模式或会话不可用"));
        return;
    }
    const int stationId = selectedStationId_;
    userApi_->navigationRoute(
        stationId, origin.latitudeE6, origin.longitudeE6, origin.keyword, mode,
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
        },
        gps);
}

void UserMainWindow::showNavigationFallback(const QString& reason)
{
    navigationMapReady_ = false;
    navigationStepsArea_->setVisible(false);
    navigationSummary_->setText(
        navigationRoute_.distance == QStringLiteral("距离暂不可用")
            ? QStringLiteral("路线暂不可用 · 请确认起点与终点")
            : QStringLiteral("路线暂不可用 · 直线距离：%1").arg(navigationRoute_.distance));
    navigationBrowserButton_->setVisible(!navigationRoute_.url.isEmpty());
    navigationBrowserButton_->setEnabled(!navigationRoute_.url.isEmpty());
    // 腾讯 URI API 的 H5 路线页不依赖 Server Key，内嵌展示即可看到完整街道级路线（与安卓端一致）。
    if (loadNavigationWebFallback())
    {
        if (!reason.isEmpty())
            navigationSummary_->setText(navigationSummary_->text() +
                                        QStringLiteral("\n内嵌展示腾讯路线页 · %1").arg(reason));
        return;
    }
    navigationMapPanel_->setCurrentIndex(0);
    navigationMapMessage_->setText(
        QStringLiteral("暂时无法显示路线\n%1\n\n请修改起点后重试，或在腾讯地图中继续")
            .arg(reason.isEmpty() ? QStringLiteral("地图服务暂不可用") : reason));
}

void UserMainWindow::applyNavigationRoute(const QJsonObject& routeData)
{
    navigationRoute_.stationName = routeData.value(QStringLiteral("stationName")).toString();
    navigationRoute_.destinationAddress =
        routeData.value(QStringLiteral("destinationAddress")).toString();
    navigationRoute_.mode = routeData.value(QStringLiteral("mode")).toString();
    navigationRoute_.url = routeData.value(QStringLiteral("browserUrl")).toString();
    const auto distance = routeData.value(QStringLiteral("distanceMeter")).toVariant().toLongLong();
    const auto duration =
        routeData.value(QStringLiteral("durationSecond")).toVariant().toLongLong();
    navigationRoute_.distance = navigationDistance(qMax<qint64>(0, distance));
    const bool fallback = routeData.value(QStringLiteral("routeFallback")).toBool();
    const QJsonArray steps = routeData.value(QStringLiteral("steps")).toArray();
    QStringList stepLines;
    for (const auto& value : steps)
    {
        const QJsonObject step = value.toObject();
        const QString instruction = step.value(QStringLiteral("instruction")).toString().trimmed();
        if (instruction.isEmpty())
            continue;
        const auto meter = step.value(QStringLiteral("distanceMeter")).toVariant().toLongLong();
        QString line = QStringLiteral("• %1").arg(instruction.toHtmlEscaped());
        if (meter > 0)
            line += QStringLiteral("  ·  %1 m").arg(meter);
        stepLines.append(line);
    }
    navigationStepsArea_->setVisible(false);
    if (fallback || distance <= 1 || duration <= 0 ||
        !usableNavigationPolyline(routeData.value(QStringLiteral("polyline")).toArray()))
    {
        showNavigationFallback(distance <= 5
                                   ? QStringLiteral("起点与电站很近，请确认起点是否正确")
                                   : QStringLiteral("地图服务没有返回可绘制的路线，请重试"));
        if (!fallback)
            navigationSummary_->setText(QStringLiteral("路线数据不完整，请重新规划"));
    }
    else
    {
        navigationSummary_->setText(QStringLiteral("%1  ·  约 %2 分钟  ·  %3")
                                        .arg(navigationRoute_.distance)
                                        .arg((duration + 59) / 60)
                                        .arg(navigationMode_->currentText()));
        if (!stepLines.isEmpty())
        {
            navigationStepsText_->setText(stepLines.join(QStringLiteral("<br>")));
            navigationStepsArea_->setVisible(true);
        }
        renderNavigationMap(routeData);
    }
    if (routeData.value(QStringLiteral("locationFallback")).toBool())
        navigationSummary_->setText(navigationSummary_->text() +
                                    QStringLiteral("\n起点地址定位失败：当前使用默认模拟位置"));
}

void UserMainWindow::renderNavigationMap(const QJsonObject& routeData)
{
#ifdef NCS_HAS_WEBENGINE
    if (tencentMapJsKey_.trimmed().isEmpty())
    {
        if (loadNavigationWebFallback())
            return;
        navigationMapPanel_->setCurrentIndex(0);
        navigationMapMessage_->setText(
            QStringLiteral("路线已规划，内嵌地图未配置\n可在腾讯地图中继续导航"));
        navigationBrowserButton_->show();
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
<script>function fail(){document.title='NCS_MAP_ERROR___ID__';const e=document.getElementById('error');e.textContent='内嵌地图不可用，请使用浏览器导航';e.style.display='block'}</script>
<script src="https://map.qq.com/api/gljs?v=1&key=__KEY__" onerror="fail()"></script>
<script>
try {
 const route=__ROUTE__;
 const points=route.map(p=>new TMap.LatLng(p.latitudeE6/1e6,p.longitudeE6/1e6));
 const map=new TMap.Map(document.getElementById('map'),{center:points[0],zoom:12});
 new TMap.MultiPolyline({map:map,styles:{route:new TMap.PolylineStyle({color:'#23794E',width:7,borderWidth:2,borderColor:'#ffffff',lineCap:'round'})},geometries:[{id:'route',styleId:'route',paths:points}]});
 new TMap.MultiMarker({map:map,geometries:[{id:'origin',position:points[0]},{id:'destination',position:points[points.length-1]}]});
 const bounds=new TMap.LatLngBounds(); points.forEach(p=>bounds.extend(p)); map.fitBounds(bounds,{padding:32}); document.title='NCS_MAP_READY___ID__';
} catch(e) { fail(); }
</script></body></html>)HTML");
    html.replace(QStringLiteral("__ID__"), QString::number(navigationRequestId_));
    html.replace(QStringLiteral("__KEY__"), QString::fromLatin1(key));
    html.replace(QStringLiteral("__ROUTE__"), QString::fromUtf8(routeJson));

    auto* map = static_cast<QWebEngineView*>(navigationMap_);
    const QUrl origin(tencentMapJsOrigin_.isEmpty() ? QStringLiteral("http://localhost/")
                                                    : tencentMapJsOrigin_);
    navigationMapPanel_->setCurrentIndex(1);
    map->setHtml(html, origin);
    const int requestId = navigationRequestId_;
    QTimer::singleShot(10000, this,
                       [this, requestId]
                       {
                           if (requestId != navigationRequestId_ || pages_->currentIndex() != 7 ||
                               navigationMapReady_)
                               return;
                           if (loadNavigationWebFallback())
                               return;
                           navigationMapPanel_->setCurrentIndex(0);
                           navigationMapMessage_->setText(QStringLiteral(
                               "底图加载超时\n路线信息仍可查看，请重试或在腾讯地图中继续"));
                           navigationBrowserButton_->show();
                       });
#else
    Q_UNUSED(routeData)
    navigationMapPanel_->setCurrentIndex(0);
    navigationMapMessage_->setText(
        QStringLiteral("此版本未包含内嵌地图组件\n可在腾讯地图中继续导航"));
    navigationBrowserButton_->show();
#endif
}

bool UserMainWindow::loadNavigationWebFallback()
{
#ifdef NCS_HAS_WEBENGINE
    const QUrl target(navigationRoute_.url);
    if (!target.isValid() || target.scheme() != QStringLiteral("https") ||
        target.host() != QStringLiteral("apis.map.qq.com"))
        return false;
    auto* map = static_cast<QWebEngineView*>(navigationMap_);
    navigationMapReady_ = false;
    navigationMapPanel_->setCurrentIndex(1);
    map->setUrl(target);
    return true;
#else
    return false;
#endif
}
} // namespace ncs::user
