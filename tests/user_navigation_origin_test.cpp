#include "apps/user/net/user_api.h"
#include "apps/user/ui/navigation_origin.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <iostream>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    using namespace ncs::user;
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost))
        return 1;
    ApiClient client(QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort())));
    UserApi api(client);
    QUrl received;
    QObject::connect(
        &server, &QTcpServer::newConnection, &app,
        [&]
        {
            auto* socket = server.nextPendingConnection();
            QObject::connect(
                socket, &QTcpSocket::readyRead, socket,
                [&, socket]
                {
                    auto pending = socket->property("pending").toByteArray() + socket->readAll();
                    socket->setProperty("pending", pending);
                    if (!pending.contains("\r\n\r\n"))
                        return;
                    received = QUrl::fromEncoded(pending.split(' ').value(1));
                    const QByteArray body = R"({"code":0,"data":{}})";
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: "
                                  "application/json\r\nConnection: close\r\nContent-Length: " +
                                  QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
    for (const auto& text : {QStringLiteral("望京（模拟位置）"), QStringLiteral(" 北京南站 "),
                             QStringLiteral("当前位置（自动定位）")})
    {
        auto origin = navigationOriginForText(text);
        const bool gps = text == QStringLiteral("当前位置（自动定位）");
        if (gps)
        {
            if (origin.latitudeE6 || origin.longitudeE6 || !origin.keyword.isEmpty())
                return 1;
            origin.latitudeE6 = 39900000;
            origin.longitudeE6 = 116450000;
        }
        received.clear();
        QEventLoop loop;
        bool completed = false;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        api.navigationRoute(
            3, origin.latitudeE6, origin.longitudeE6, origin.keyword, QStringLiteral("driving"),
            [&](ApiReply)
            {
                completed = true;
                loop.quit();
            },
            gps);
        timer.start(2000);
        loop.exec();
        const QUrlQuery query(received);
        if (!completed || received.path() != QStringLiteral("/api/v1/user/stations/3/route"))
            return 1;
        if (gps && (query.queryItemValue("coordinateType") != "wgs84" ||
                    query.queryItemValue("latitudeE6") != "39900000"))
            return 1;
        if (!gps && origin.latitudeE6 &&
            (query.queryItemValue("latitudeE6") != "39993300" ||
             query.queryItemValue("longitudeE6") != "116473200"))
            return 1;
        if (!origin.latitudeE6 &&
            (query.hasQueryItem("latitudeE6") || query.hasQueryItem("longitudeE6") ||
             query.queryItemValue("keyword") != QStringLiteral("北京南站")))
            return 1;
    }
    const QJsonObject a{{"latitudeE6", 39900000}, {"longitudeE6", 116400000}};
    const QJsonObject b{{"latitudeE6", 39910000}, {"longitudeE6", 116410000}};
    if (!usableNavigationPolyline({a, b}) || usableNavigationPolyline({a, a}) ||
        usableNavigationPolyline({a}) || usableNavigationPolyline({a, QJsonObject{}}) ||
        usableNavigationPolyline({a, QJsonObject{{"latitudeE6", 91000000}, {"longitudeE6", 0}}}) ||
        navigationDistance(1) != QStringLiteral("1 米"))
    {
        std::cerr << "FAIL: invalid map geometry or misleading distance format\n";
        return 1;
    }
    return 0;
}
