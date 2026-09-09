#include "../core/message_frame.h"
#include <QtTest>
#include <limits>
using namespace ncs::device_sim;
class MessageFrameTest : public QObject
{
    Q_OBJECT
  private slots:
    void rejectInvalidIds()
    {
        Frame f;
        for (const auto value :
             {QJsonValue(0), QJsonValue(-1), QJsonValue(1.5), QJsonValue(1e30), QJsonValue("1")})
            QVERIFY(!MessageFrame::decode({2, value, "Heartbeat", QJsonObject{}}, f));
        const auto largest = std::numeric_limits<qint64>::max();
        QVERIFY(MessageFrame::decode(MessageFrame::encodeCall(largest, "Heartbeat", {}), f));
        QCOMPARE(f.messageId, largest);
    }
    void roundTrip()
    {
        Frame f;
        QVERIFY(MessageFrame::decode(
            MessageFrame::encodeCall(7, "BootNotification", {{"deviceId", "P-01"}}), f));
        QCOMPARE(f.messageId, qint64(7));
        QCOMPARE(f.action, QString("BootNotification"));
    }
    void rejectInvalid()
    {
        Frame f;
        QString e;
        QVERIFY(!MessageFrame::decode({2, 1, "BootNotification"}, f, &e));
        QVERIFY(!e.isEmpty());
        QVERIFY(!MessageFrame::decode({9, 1, {}}, f, &e));
    }
    void errorFrame()
    {
        Frame f;
        QVERIFY(MessageFrame::decode(MessageFrame::encodeError(3, "OFFLINE", "device disconnected"),
                                     f));
        QCOMPARE(f.kind, Frame::Kind::Error);
        QCOMPARE(f.errorCode, QString("OFFLINE"));
    }
};
QTEST_MAIN(MessageFrameTest)
#include "message_frame_test.moc"
