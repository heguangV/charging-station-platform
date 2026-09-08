#include "ui/app_theme.h"
#include "ui/charger_table.h"
#include "ui/station_card.h"
#include "user_main_window.h"
#include <QApplication>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTest>

using namespace ncs::user;
class StationLayoutTest : public QObject
{
    Q_OBJECT
    QPushButton* button(QWidget& root, const QString& text)
    {
        for (auto* b : root.findChildren<QPushButton*>())
            if (b->isVisibleTo(&root) && b->text().contains(text))
                return b;
        return nullptr;
    }
  private slots:
    void initTestCase()
    {
        AppTheme::apply(*qApp);
    }
    void selectionSurvivesRefresh()
    {
        ChargerTable table;
        QVector<ChargerSummary> data;
        for (int i = 1; i <= 6; ++i)
            data.append({QString("STATION-DC-0%1").arg(i), QStringLiteral("快充"), 120,
                         QStringLiteral("空闲"), 20, i, 1});
        data[0].status = QStringLiteral("故障");
        table.setChargers(data);
        table.resize(340, 600);
        table.show();
        QApplication::processEvents();
        auto* choice = button(table, QStringLiteral("选择"));
        QVERIFY(choice);
        QTest::mouseClick(choice, Qt::LeftButton);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!table.selectedChargerCode().isEmpty());
        const auto selected = table.selectedChargerCode();
        table.setChargers(data);
        QCOMPARE(table.selectedChargerCode(), selected);
        QTest::qWait(80);
        choice = button(table, QStringLiteral("取消选择"));
        QVERIFY(choice);
        QTest::mouseClick(choice, Qt::LeftButton);
        QVERIFY(table.selectedChargerCode().isEmpty());
        QTest::qWait(80);
        auto* more = button(table, QStringLiteral("展开更多"));
        QVERIFY(more);
        QTest::mouseClick(more, Qt::LeftButton);
        QApplication::processEvents();
        choice = nullptr;
        for (auto* b : table.findChildren<QPushButton*>())
            if (b->isVisibleTo(&table) && b->accessibleName().contains("DC-06"))
                choice = b;
        QVERIFY(choice);
        QTest::mouseClick(choice, Qt::LeftButton);
        QTest::qWait(80);
        auto* collapse = button(table, QStringLiteral("收起"));
        QVERIFY(collapse);
        QTest::mouseClick(collapse, Qt::LeftButton);
        QCOMPARE(table.selectedChargerCode(), QString("STATION-DC-06"));
        data[5].status = QStringLiteral("使用中");
        table.setChargers(data);
        QVERIFY(table.selectedChargerCode().isEmpty());
        data[0].code = QString(50, 'A');
        table.setChargers(data);
        QApplication::processEvents();
        QVERIFY(table.minimumSizeHint().width() <= 340);
        table.setChargers({});
        QApplication::processEvents();
        QVERIFY(!button(table, QStringLiteral("展开更多")));
    }
    void detailPageLayout()
    {
        MockUserClientService service;
        QString message;
        QVERIFY(service.configureScenario("happy-path", &message));
        UserMainWindow window(service, nullptr, {}, {});
        window.show();
        QApplication::processEvents();
        for (auto* field : window.findChildren<QLineEdit*>())
        {
            if (field->placeholderText().contains(QStringLiteral("手机号")))
                field->setText("19900000001");
            if (field->placeholderText().contains(QStringLiteral("验证码")))
                field->setText("123456");
        }
        auto* login = button(window, QStringLiteral("登录"));
        QVERIFY(login);
        login->click();
        QApplication::processEvents();
        auto* station = window.findChild<StationCard*>();
        QVERIFY(station);
        emit station->selected(1);
        QApplication::processEvents();
        auto* pages = window.findChild<QStackedWidget*>();
        QVERIFY(pages);
        QCOMPARE(pages->currentIndex(), 2);
        auto* reserve = button(window, QStringLiteral("请选择空闲电桩"));
        QVERIFY(reserve);
        QVERIFY(!reserve->isEnabled());
        auto* table = window.findChild<ChargerTable*>();
        QVERIFY(table);
        auto* choice = button(*table, QStringLiteral("选择"));
        QVERIFY(choice);
        choice->click();
        QTest::qWait(100);
        QVERIFY(reserve->isEnabled());
        QCOMPARE(reserve->text(), QStringLiteral("预约所选电桩"));
        QCOMPARE(window.size(), QSize(420, 760));
        auto* scroll = pages->currentWidget()->findChild<QScrollArea*>();
        QVERIFY(scroll);
        QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        QTest::qWait(2900);
        QDir().mkpath("/tmp/ncs-layout-evidence");
        QVERIFY(window.grab().save("/tmp/ncs-layout-evidence/desktop-detail.png"));
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
        QApplication::processEvents();
        QVERIFY(window.grab().save("/tmp/ncs-layout-evidence/desktop-detail-bottom.png"));
        QVERIFY(reserve->isVisibleTo(&window));
    }
};
QTEST_MAIN(StationLayoutTest)
#include "station_layout_test.moc"
