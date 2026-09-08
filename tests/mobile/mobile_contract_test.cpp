#include "mobile_api.h"
#include <QBuffer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

class MobileContractTest : public QObject
{
    Q_OBJECT
  private slots:
    void contractsAndRecovery()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QList<QByteArray> requests;
        QByteArray rechargeKey;
        int rechargeCalls = 0;
        bool smsFails = false;
        bool avatarFails = false;
        bool unauthorized = false;
        connect(
            &server, &QTcpServer::newConnection, this,
            [&]
            {
                auto* socket = server.nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(
                    socket, &QTcpSocket::readyRead, this,
                    [&, socket]
                    {
                        QByteArray raw = socket->property("raw").toByteArray() + socket->readAll();
                        socket->setProperty("raw", raw);
                        const int end = raw.indexOf("\r\n\r\n");
                        if (end < 0)
                            return;
                        int size = 0;
                        for (auto line : raw.left(end).split('\n'))
                            if (line.toLower().startsWith("content-length:"))
                                size = line.mid(15).trimmed().toInt();
                        if (raw.size() < end + 4 + size || socket->property("handled").toBool())
                            return;
                        socket->setProperty("handled", true);
                        requests.append(raw);
                        const QByteArray path = raw.split(' ')[1];
                        QJsonObject data;
                        int status = 200;
                        QByteArray binary;
                        if (unauthorized)
                            status = 401;
                        else if (path.endsWith("/auth/sms/code"))
                        {
                            status = smsFails ? 503 : 200;
                            data = {{"developmentCode", "123456"}};
                        }
                        else if (path.endsWith("/login/sms"))
                            data = {{"accessToken", "test-token"}};
                        else if (path.endsWith("/me"))
                            data = {{"nickname", "测试"},
                                    {"phoneMasked", "199****0001"},
                                    {"balanceCent", 10000},
                                    {"version", 7},
                                    {"registeredAt", 1788820000}};
                        else if (path.contains("/wallet/recharges"))
                        {
                            ++rechargeCalls;
                            status = rechargeCalls == 1 ? 503 : 200;
                            for (auto line : raw.left(end).split('\n'))
                                if (line.toLower().startsWith("idempotency-key:"))
                                {
                                    if (rechargeKey.isEmpty())
                                        rechargeKey = line.trimmed();
                                    else
                                        QCOMPARE(line.trimmed(), rechargeKey);
                                }
                            QCOMPARE(QJsonDocument::fromJson(raw.mid(end + 4))
                                         .object()
                                         .value("amountCent")
                                         .toInt(),
                                     1234);
                        }
                        else if (path.endsWith("/avatar/content"))
                        {
                            QImage im(2, 2, QImage::Format_RGB32);
                            im.fill(Qt::green);
                            QBuffer buffer(&binary);
                            buffer.open(QIODevice::WriteOnly);
                            im.save(&buffer, "JPEG");
                        }
                        else if (path.endsWith("/avatar"))
                            status = avatarFails ? 503 : 200;
                        else if (path.endsWith("/flows/F1/progress"))
                            data = {{"flowNo", "F1"}, {"energyMwh", 1500000}, {"amountCent", 200}};
                        else if (path.endsWith("/flows/F1"))
                            data = {{"flowNo", "F1"}, {"status", 40}, {"version", 12}};
                        else if (path.endsWith("/flows"))
                            data = {{"flowNo", "F1"},
                                    {"status", 20},
                                    {"version", 10},
                                    {"quote", QJsonObject{{"quoteNo", "Q1"}}}};
                        else if (path.endsWith("/settlements"))
                            data = {{"orderNo", "O1"}, {"status", 60}, {"amountCent", 200}};
                        QByteArray body =
                            binary.isEmpty() ? QJsonDocument(QJsonObject{{"success", status == 200},
                                                                         {"data", data}})
                                                   .toJson(QJsonDocument::Compact)
                                             : binary;
                        const auto response =
                            "HTTP/1.1 " + QByteArray::number(status) + " OK\r\nContent-Type: " +
                            (binary.isEmpty() ? QByteArray("application/json")
                                              : QByteArray("image/jpeg")) +
                            "\r\nContent-Length: " + QByteArray::number(body.size()) +
                            "\r\nConnection: close\r\n\r\n" + body;
                        QTimer::singleShot(20, socket,
                                           [socket, response]
                                           {
                                               socket->write(response);
                                               socket->disconnectFromHost();
                                           });
                    });
            });
        qputenv("NCS_API_BASE_URL",
                QString("http://127.0.0.1:%1/api/v1").arg(server.serverPort()).toUtf8());
        ncs::mobile::MobileApi api;
        QVERIFY(!api.configureServer("http://example.com/api/v1"));
        QVERIFY(!api.configureServer("https://name:password@example.com"));
        QSignalSpy code(&api, &ncs::mobile::MobileApi::codeSent);
        api.requestCode("123");
        QCOMPARE(requests.size(), 0);
        smsFails = true;
        api.requestCode("19900000001");
        QTRY_VERIFY(!api.busy());
        QCOMPARE(code.count(), 0);
        QVERIFY(api.messageError());
        smsFails = false;
        api.requestCode("19900000001");
        QTRY_COMPARE(code.count(), 1);
        api.login("19900000001", "123456");
        QTRY_VERIFY(api.loggedIn());
        QTRY_VERIFY(!api.busy());
        QCOMPARE(api.nickname(), QStringLiteral("测试"));
        QVERIFY(!api.createdAt().startsWith("1970"));
        QVERIFY(!api.configureServer("https://example.com"));
        api.recharge("12.345");
        QVERIFY(!api.busy());
        QCOMPARE(rechargeCalls, 0);
        api.recharge("12.34");
        api.recharge("12.34");
        QTRY_VERIFY(!api.busy());
        QCOMPARE(rechargeCalls, 1);
        api.recharge("12.34");
        QTRY_COMPARE(rechargeCalls, 2);
        QTRY_VERIFY(!api.busy());
        QVERIFY(!rechargeKey.isEmpty());
        api.updateNickname("新昵称");
        QTRY_VERIFY(!api.busy());
        bool foundPut = false;
        for (const auto& raw : requests)
            if (raw.startsWith("PUT "))
            {
                foundPut = true;
                QCOMPARE(QJsonDocument::fromJson(raw.mid(raw.indexOf("\r\n\r\n") + 4))
                             .object()
                             .value("version")
                             .toInt(),
                         7);
            }
        QVERIFY(foundPut);
        QImage capture(1800, 1200, QImage::Format_RGB32);
        capture.fill(Qt::red);
        QVERIFY(capture.save(api.avatarCapturePath(), "JPEG"));
        api.uploadAvatar(api.avatarCapturePath());
        QTRY_VERIFY(!api.avatarUploading());
        QTRY_VERIFY(!api.avatarData().isEmpty());
        QVERIFY(!QFile::exists(api.avatarCapturePath()));
        QByteArray upload;
        for (auto raw : requests)
            if (raw.startsWith("POST /api/v1/user/me/avatar "))
                upload = raw;
        QVERIFY(!upload.isEmpty());
        QVERIFY(upload.size() < 200 * 1024 + 1024);
        const int jpeg = upload.indexOf(QByteArray::fromHex("ffd8"));
        QVERIFY(jpeg > 0);
        const auto decoded = QImage::fromData(upload.mid(jpeg), "JPEG");
        QCOMPARE(decoded.size(), QSize(512, 512));
        const auto oldAvatar = api.avatarData();
        avatarFails = true;
        QVERIFY(capture.save(api.avatarCapturePath(), "JPEG"));
        api.uploadAvatar(api.avatarCapturePath());
        QTRY_VERIFY(!api.avatarUploading());
        QCOMPARE(api.avatarData(), oldAvatar);
        QVERIFY(api.messageError());
        api.requestCharge(1, 1, 1);
        QTRY_VERIFY(!api.busy());
        QCOMPARE(api.flow().value("version").toInt(), 10);
        api.loadFlowProgress();
        QTRY_VERIFY(!api.busy());
        QCOMPARE(api.flow().value("version").toInt(), 12);
        QCOMPARE(api.flow().value("energyMwh").toLongLong(), 1500000);
        api.settleCharge();
        QTRY_VERIFY(!api.busy());
        QVERIFY(api.flow().isEmpty());
        QCOMPARE(api.receipt().value("orderNo").toString(), QString("O1"));
        unauthorized = true;
        api.loadProfile();
        QTRY_VERIFY(!api.loggedIn());
        QCOMPARE(api.balanceCent(), 0);
        QVERIFY(api.avatarData().isEmpty());
    }
};
QTEST_MAIN(MobileContractTest)
#include "mobile_contract_test.moc"
