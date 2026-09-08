#include <QDir>
#include <QGuiApplication>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickView>
#include <QTest>
#include <QVariantMap>

class MobileStationLayoutTest : public QObject
{
    Q_OBJECT
    QQuickItem* findVisual(QQuickItem* item, const QString& name)
    {
        if (item->objectName() == name)
            return item;
        for (auto* child : item->childItems())
            if (auto* found = findVisual(child, name))
                return found;
        return nullptr;
    }
  private slots:
    void sizesAndStates()
    {
        QQuickView view;
        view.setResizeMode(QQuickView::SizeRootObjectToView);
        view.setSource(QUrl::fromLocalFile(QStringLiteral(NCS_STATION_QML)));
        QVERIFY2(
            view.status() == QQuickView::Ready,
            qPrintable(view.errors().isEmpty() ? QString() : view.errors().first().toString()));
        auto* page = view.rootObject();
        QVERIFY(page);
        page->setProperty(
            "station",
            QVariantMap{
                {"name", QStringLiteral("NCS 长名称充电站·新能源示范服务中心")},
                {"address", QStringLiteral("北京市海淀区中关村大街 27 号地下二层停车场 C 区")},
                {"distanceMeter", 3500},
                {"totalPriceCentPerKwh", 135},
                {"electricityPriceCentPerKwh", 100},
                {"servicePriceCentPerKwh", 35}});
        QVariantList chargers;
        for (int i = 1; i <= 6; ++i)
            chargers.append(QVariantMap{
                {"id", i},
                {"code", QString("ZGC-DC-%1").arg(i)},
                {"status", i == 1 ? 2 : 0},
                {"statusText", i == 1 ? QStringLiteral("故障") : QStringLiteral("空闲")},
                {"chargerType", 1},
                {"powerWatt", 120000},
                {"totalCount", 200}});
        page->setProperty("chargers", chargers);
        view.resize(360, 720);
        view.show();
        QTest::qWait(150);
        auto* reserve = page->findChild<QQuickItem*>("reserveButton");
        QVERIFY(reserve);
        QVERIFY(!reserve->isEnabled());
        page->setProperty("selectedId", 2);
        QVERIFY(reserve->isEnabled());
        page->setProperty("busy", true);
        QVERIFY(!reserve->isEnabled());
        page->setProperty("busy", false);
        QDir().mkpath("/tmp/ncs-layout-evidence");
        for (int width : {360, 420})
        {
            view.resize(width, 760);
            QTest::qWait(100);
            QVERIFY(view.grabWindow().save(
                QString("/tmp/ncs-layout-evidence/android-detail-%1.png").arg(width)));
            const auto point = reserve->mapToScene(QPointF(0, 0));
            QVERIFY(point.x() >= 0);
            QVERIFY(point.x() + reserve->width() <= width + 1);
            QVERIFY(point.y() + reserve->height() <= view.height());
        }
        page->setProperty("expanded", true);
        QTest::qWait(50);
        QVERIFY(findVisual(page, "chargerChoice6"));
        page->setProperty("selectedId", 6);
        page->setProperty("expanded", false);
        QVERIFY(reserve->isEnabled());
        page->setProperty("chargers", QVariantList{});
        page->setProperty("selectedId", 0);
        QVERIFY(!reserve->isEnabled());
        page->setProperty("errorMessage", QStringLiteral("网络不可用，请刷新重试"));
        QTest::qWait(80);
        QVERIFY(view.grabWindow().save("/tmp/ncs-layout-evidence/android-detail-error.png"));
        page->setProperty("hasActiveFlow", true);
        QVERIFY(reserve->isEnabled());
    }
};
int main(int argc, char** argv)
{
    QQuickStyle::setStyle("Basic");
    QGuiApplication app(argc, argv);
    MobileStationLayoutTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "mobile_station_layout_test.moc"
