#include "admin_http_fixture.h"
#include "admin_main_window.h"
#include "admin_main_window_utils.h"
#include "admin_page_status.h"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <iostream>
using namespace ncs::admin;
namespace
{
void waitFor(const std::function<bool()>& predicate, const char* message)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 5000)
    {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    require(predicate(), message);
}
template <class T> T* widget(QObject& parent, const char* name)
{
    auto* value = parent.findChild<T*>(QString::fromLatin1(name));
    require(value != nullptr, name);
    return value;
}
void invoke(AdminMainWindow& window, const char* slot)
{
    require(QMetaObject::invokeMethod(&window, slot, Qt::DirectConnection), slot);
}
void screenshot(AdminMainWindow& window, const QString& name)
{
    const auto folder = qEnvironmentVariable("NCS_ADMIN_SCREENSHOT_DIR");
    if (folder.isEmpty())
        return;
    QDir().mkpath(folder);
    QApplication::processEvents();
    require(window.grab().save(folder + "/" + name + ".png"), "save screenshot");
}
void login(AdminMainWindow& window)
{
    widget<QLineEdit>(window, "adminUsername")->setText("operator");
    widget<QLineEdit>(window, "adminPassword")->setText("fixture-password");
    widget<QPushButton>(window, "adminLoginButton")->click();
}
void checkWindow(AdminMainWindow& window)
{
    require(window.size() == QSize(1040, 680), "minimum window expanded");
    for (auto* table : window.findChildren<QTableWidget*>())
        if (table->isVisible())
            require(table->width() > 500, "table compressed");
}
void contracts()
{
    require(!AdminApiClient::parseReply(200, QNetworkReply::NoError, "{}").ok(),
            "malformed envelope accepted");
    require(!AdminApiClient::parseReply(200, QNetworkReply::NoError,
                                        R"({"success":false,"code":0,"data":{}})")
                 .ok(),
            "failed envelope accepted");
    require(!AdminApiClient::parseReply(302, QNetworkReply::NoError,
                                        R"({"success":true,"code":0,"data":{}})")
                 .ok(),
            "redirect accepted");
    const auto day =
        QDateTime(QDate(2026, 9, 7), QTime(0, 0), businessTimeZone()).toSecsSinceEpoch();
    const auto grouped = groupRevenueByDay(QJsonArray{
        QJsonObject{{"bucketStart", day - 3600}, {"amountCent", 100}, {"orderCount", 1}},
        QJsonObject{{"bucketStart", day}, {"amountCent", 200}, {"orderCount", 2}},
        QJsonObject{{"bucketStart", day + 3600}, {"amountCent", 300}, {"orderCount", 3}}});
    require(grouped.size() == 2 && grouped[1].amountCent == 500 && grouped[1].orders == 5,
            "Beijing midnight revenue aggregation");
}
void interfaceFlow()
{
    AdminHttpFixture server;
    AdminApiClient api(server.baseUrl());
    AdminMainWindow window(api, "development");
    window.show();
    screenshot(window, "admin-login");
    login(window);
    auto* root = widget<QStackedWidget>(window, "adminRootPages");
    auto* nav = widget<QListWidget>(window, "adminNavigation");
    waitFor(
        [&] {
            return root->currentIndex() == 1 &&
                   widget<QLabel>(window, "registeredUsers")->text() == "51";
        },
        "login and overview");
    require(widget<QLabel>(window, "todayRevenue")->text() == QString::fromUtf8("¥123.45"),
            "revenue cents contract");
    require(widget<QLabel>(window, "monthRevenue")->text() == QString::fromUtf8("¥123.45"),
            "month cents contract");
    require(widget<QLabel>(window, "chargerHealth")->text().contains("87.5%"),
            "server healthPercent ignored");
    int today = 0, month = 0;
    const auto now = QDateTime::currentDateTimeUtc().toTimeZone(businessTimeZone());
    for (const auto& r : server.requests)
        if (r.url.path().endsWith("/stats/revenue"))
        {
            const QUrlQuery q(r.url);
            const auto from = q.queryItemValue("fromAt").toLongLong();
            if (from == QDateTime(now.date(), QTime(0, 0), businessTimeZone()).toSecsSinceEpoch())
                ++today;
            if (from == QDateTime(QDate(now.date().year(), now.date().month(), 1), QTime(0, 0),
                                  businessTimeZone())
                            .toSecsSinceEpoch())
                ++month;
        }
    require(today > 0 && month > 0, "today/month query boundaries");
    screenshot(window, "admin-overview");
    window.resize(1040, 680);
    QApplication::processEvents();
    checkWindow(window);
    screenshot(window, "admin-overview-compact");
    window.resize(1360, 880);
    nav->setCurrentRow(1);
    auto* stations = widget<QTableWidget>(window, "stationTable");
    waitFor([&] { return stations->rowCount() == 1; }, "station list");
    require(stations->item(0, 1)->text() == QStringLiteral("中关村绿色能源站"), "station metadata");
    screenshot(window, "admin-stations");
    nav->setCurrentRow(2);
    auto* chargers = widget<QTableWidget>(window, "chargerTable");
    waitFor([&] { return chargers->rowCount() == 1; }, "charger list");
    require(chargers->item(0, 0)->data(Qt::UserRole).toLongLong() == 42, "stable charger ID");
    require(chargers->item(0, 1)->text() == QStringLiteral("中关村绿色能源站"),
            "station name join");
    require(chargers->item(0, 2)->text().contains(QStringLiteral("直流")), "numeric charger type");
    require(chargers->item(0, 4)->text() == QStringLiteral("使用中"),
            "occupied status normalization");
    widget<QComboBox>(window, "chargerStatus")->setCurrentIndex(2);
    invoke(window, "refreshChargers");
    waitFor([&] { return chargers->rowCount() == 1; }, "filter charger");
    require(QUrlQuery(server.requests.last().url).queryItemValue("status") == "1",
            "status server filter");
    screenshot(window, "admin-chargers");
    // Drive actual modal confirmations; the UI must still wait for server replies.
    QTimer dialogs;
    QObject::connect(
        &dialogs, &QTimer::timeout, &window,
        [&]
        {
            for (auto* d : window.findChildren<QDialog*>())
            {
                if (!d->isVisible())
                    continue;
                if (d->objectName() == "confirmOperation")
                {
                    widget<QLineEdit>(*d, "operationReason")->setText(QStringLiteral("验收测试"));
                    widget<QDialogButtonBox>(*d, "")->button(QDialogButtonBox::Ok)->click();
                }
                else if (auto* input = qobject_cast<QInputDialog*>(d))
                {
                    input->setTextValue("fixture-password");
                    input->accept();
                }
            }
        });
    dialogs.start(15);
    chargers->selectRow(0);
    server.wrongReauth = true;
    invoke(window, "restartCharger");
    waitFor(
        [&]
        {
            return server.count("admin/auth/reauth", "POST") == 1 &&
                   widget<QPushButton>(window, "restartCharger")->isEnabled();
        },
        "wrong reauth response");
    require(root->currentIndex() == 1 && api.hasSession(), "wrong password expired session");
    require(server.count("admin/chargers/42/restart-commands", "POST") == 0,
            "restart sent after wrong password");
    server.wrongReauth = false;
    invoke(window, "restartCharger");
    waitFor(
        [&] {
            return widget<QLabel>(window, "operationMessage")->text() ==
                   QStringLiteral("设备重启成功");
        },
        "restart polling success");
    require(server.count("admin/chargers/42/restart-commands", "POST") == 1,
            "restart used code as numeric ID");
    nav->setCurrentRow(3);
    auto* users = widget<QTableWidget>(window, "userTable");
    waitFor([&] { return users->rowCount() == 1; }, "users list");
    require(users->item(0, 5)->text() == QStringLiteral("正常"), "user numeric status");
    require(users->item(0, 3)->text() == QString::fromUtf8("¥50.25"), "balance cents");
    screenshot(window, "admin-users");
    users->selectRow(0);
    invoke(window, "toggleUserStatus");
    waitFor(
        [&]
        { return users->rowCount() == 1 && users->item(0, 5)->text() == QStringLiteral("冻结"); },
        "freeze flow");
    users->selectRow(0);
    invoke(window, "toggleUserStatus");
    waitFor(
        [&]
        { return users->rowCount() == 1 && users->item(0, 5)->text() == QStringLiteral("正常"); },
        "unfreeze flow");
    require(server.count("admin/users/9") == 2, "missing latest user version fetch");
    dialogs.stop();
    widget<QPushButton>(window, "nextPage3")->click();
    waitFor([&] { return users->rowCount() == 1; }, "next page");
    require(QUrlQuery(server.requests.last().url).queryItemValue("page") == "2",
            "pagination query");
    server.failUsers = true;
    invoke(window, "refreshUsers");
    require(users->rowCount() == 0, "stale rows retained while loading");
    auto* state = widget<AdminPageStatus>(window, "pageStatus3");
    auto* retry = widget<QPushButton>(*state, "secondaryButton");
    waitFor([&] { return retry->isVisible(); }, "persistent failure and retry");
    screenshot(window, "admin-error");
    server.failUsers = false;
    server.emptyUsers = true;
    retry->click();
    waitFor(
        [&] {
            return widget<QLabel>(*state, "pageStateText")->text() ==
                   QStringLiteral("未找到匹配用户");
        },
        "empty result state");
    require(users->rowCount() == 0, "empty result retained rows");
    server.emptyUsers = false;
    invoke(window, "refreshUsers");
    waitFor([&] { return users->rowCount() == 1; }, "recovery");
    nav->setCurrentRow(4);
    auto* predictions = widget<QTableWidget>(window, "predictionTable");
    waitFor([&] { return predictions->rowCount() == 1; }, "predictions list");
    require(predictions->item(0, 0)->text() == "2024-01-01 08:00", "prediction time units");
    require(predictions->item(0, 3)->text() == "8", "predictedIdleCount");
    require(predictions->item(0, 4)->text() == QStringLiteral("高峰"), "boolean peakFlag");
    require(predictions->item(0, 5)->text() == QStringLiteral("已过期"), "staleFlag");
    screenshot(window, "admin-predictions");
    invoke(window, "runPrediction");
    waitFor(
        [&]
        {
            return server.count("admin/ml-tasks/task-test") > 0 &&
                   widget<QPushButton>(window, "runPrediction")->isEnabled();
        },
        "prediction task polling");
    server.expire = true;
    invoke(window, "refreshPredictions");
    waitFor([&] { return root->currentIndex() == 0; }, "expired session login guidance");
    require(!api.hasSession() && predictions->rowCount() == 0, "session data not cleared");
    server.expire = false;
    server.mustChange = true;
    login(window);
    waitFor(
        [&]
        {
            auto* d = window.findChild<QDialog*>("changePasswordDialog");
            return d && d->isVisible();
        },
        "required password change ignored");
    require(root->currentIndex() == 0, "workspace accessible before password change");
    auto* password = widget<QDialog>(window, "changePasswordDialog");
    widget<QLineEdit>(*password, "currentPassword")->setText("fixture-password");
    widget<QLineEdit>(*password, "newPassword")->setText("fixture-new-password");
    widget<QLineEdit>(*password, "confirmPassword")->setText("fixture-new-password");
    widget<QDialogButtonBox>(*password, "")->button(QDialogButtonBox::Save)->click();
    waitFor([&] { return root->currentIndex() == 1; }, "required password change completion");
    invoke(window, "logout");
    waitFor([&] { return root->currentIndex() == 0; }, "logout endpoint");
    require(server.count("admin/auth/logout", "POST") == 1, "logout was local only");
}
} // namespace
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setStyle("Fusion");
    try
    {
        contracts();
        interfaceFlow();
        std::cout << "Admin UI contracts passed\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
