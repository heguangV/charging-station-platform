#include "user_main_window.h"

#include "ui/bottom_navigation.h"
#include "ui/station_map_bridge.h"
#include "ui/station_map_widget.h"

#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>

#ifdef NCS_HAS_WEBENGINE
#include <QWebChannel>
#include <QWebEngineView>
#endif

namespace ncs::user
{
namespace
{
QLabel* label(const QString& value, int size = 14)
{
    auto* result = new QLabel(value);
    result->setWordWrap(true);
    result->setStyleSheet(QStringLiteral("font-size:%1px;color:#243F30;font-weight:%2;")
                              .arg(size)
                              .arg(size >= 20 ? 700 : 400));
    return result;
}
} // namespace

QWidget* UserMainWindow::createStationMapPage()
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 8, 0, 0);
    layout->setSpacing(10);
    auto* title = label(QStringLiteral("电站地图"), 23);
    title->setStyleSheet(QStringLiteral("font-size:23px;color:#243F30;font-weight:700;"));
    auto* hint = label(QStringLiteral("标记显示空闲电桩数量；点击标记查看并预约"), 13);
    hint->setStyleSheet(QStringLiteral("font-size:13px;color:#607362;"));
    stationMap_ = new StationMapWidget;
    auto* back = button(
        QStringLiteral("返回站点列表"),
        QStringLiteral("QPushButton{background:#E4F0DC;color:#23794E;border:0;border-radius:10px;"
                       "font-size:15px;font-weight:600;}"));
    layout->addWidget(title);
    layout->addWidget(hint);
    layout->addWidget(stationMap_, 1);
#ifdef NCS_HAS_WEBENGINE
    auto* realMap = new QWebEngineView;
    realMap->setVisible(false);
    stationRealMap_ = realMap;
    layout->addWidget(realMap, 1);
    stationMapBridge_ = new StationMapBridge(this);
    auto* channel = new QWebChannel(this);
    channel->registerObject(QStringLiteral("bridge"), stationMapBridge_);
    realMap->page()->setWebChannel(channel);
    connect(stationMapBridge_, &StationMapBridge::stationClicked, this,
            [this](int stationId) { selectStationFromMap(stationId); });
#endif
    layout->addWidget(back);
    connect(back, &QPushButton::clicked, this, &UserMainWindow::showHome);
    stationMap_->setOnStationSelected([this](int stationId) { selectStationFromMap(stationId); });
    return page;
}

void UserMainWindow::showStationMap()
{
    bottomNavigation_->hide();
    stationMap_->setStations(stationsById_.values().toVector());
    pages_->setCurrentIndex(8);
    renderStationMap();
}

void UserMainWindow::selectStationFromMap(int stationId)
{
    const auto station = stationsById_.constFind(stationId);
    if (station == stationsById_.cend())
    {
        notify(QStringLiteral("站点信息已更新，请返回列表刷新"), true);
        return;
    }
    selectedStationDistance_ = station->distance;
    showDetail(stationId);
}

void UserMainWindow::renderStationMap()
{
#ifdef NCS_HAS_WEBENGINE
    auto* map = static_cast<QWebEngineView*>(stationRealMap_);
    if (!map || pages_->currentIndex() != 8)
        return;
    if (tencentMapJsKey_.trimmed().isEmpty() || stationsById_.isEmpty())
    {
        // 无 JS Key 或无站点时保留示意选站图，保证页面始终可用。
        map->setVisible(false);
        stationMap_->setVisible(true);
        return;
    }
    QJsonArray items;
    for (const StationSummary& station : stationsById_.values())
    {
        QJsonObject item;
        item.insert(QStringLiteral("id"), station.id);
        item.insert(QStringLiteral("name"), station.name);
        item.insert(QStringLiteral("latitude"), station.latitude);
        item.insert(QStringLiteral("longitude"), station.longitude);
        item.insert(QStringLiteral("idleCount"), station.idleCount);
        items.append(item);
    }
    QString stationsJson = QString::fromUtf8(QJsonDocument(items).toJson(QJsonDocument::Compact));
    // JSON 进入 <script> 字面量前闭合标签分隔符，防止站点名称截断脚本。
    stationsJson.replace(QStringLiteral("</"), QStringLiteral("<\\/"));
    const QByteArray key = QUrl::toPercentEncoding(tencentMapJsKey_.trimmed());
    QString html = QString::fromUtf8(R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>html,body,#map{width:100%;height:100%;margin:0}#error{display:none;position:absolute;z-index:2;left:8px;right:8px;top:8px;padding:8px;background:#fee2e2;color:#991b1b;border-radius:6px}</style>
</head><body><div id="map"></div><div id="error"></div>
<script>function fail(){const e=document.getElementById('error');e.textContent='内嵌地图不可用，请稍后重试';e.style.display='block'}</script>
<script src="https://map.qq.com/api/gljs?v=1&key=__KEY__" onerror="fail()"></script>
<script src="qrc:///qtwebchannel/qwebchannel.js"></script>
<script>
let bridge=null;
if (window.qt && qt.webChannelTransport)
 new QWebChannel(qt.webChannelTransport, ch=>{ bridge=ch.objects.bridge; });
function pickStation(id){ if (bridge) bridge.selectStation(id); }
try {
 const stations=__STATIONS__;
 const map=new TMap.Map(document.getElementById('map'),{center:new TMap.LatLng(stations[0].latitude,stations[0].longitude),zoom:13});
 const bounds=new TMap.LatLngBounds();
 const geometries=stations.map(s=>{
  const position=new TMap.LatLng(s.latitude,s.longitude);
  bounds.extend(position);
  return {id:'s'+s.id, position:position,
          content:s.idleCount>0?('空闲 '+s.idleCount):'已满',
          properties:{stationId:s.id}};
 });
 const markers=new TMap.MultiMarker({map:map, geometries:geometries});
 markers.on('click', evt=>{
  if (!evt || !evt.geometry) return;
  const id=(evt.geometry.properties&&evt.geometry.properties.stationId)||
           parseInt(String(evt.geometry.id).slice(1),10);
  if (id) pickStation(id);
 });
 if (stations.length>1) map.fitBounds(bounds,{padding:56});
} catch(e) { fail(); }
</script></body></html>)HTML");
    html.replace(QStringLiteral("__KEY__"), QString::fromLatin1(key));
    html.replace(QStringLiteral("__STATIONS__"), stationsJson);
    const QUrl origin(tencentMapJsOrigin_.isEmpty() ? QStringLiteral("http://localhost/")
                                                    : tencentMapJsOrigin_);
    map->setHtml(html, origin);
    map->setVisible(true);
    stationMap_->setVisible(false);
#endif
}

} // namespace ncs::user
