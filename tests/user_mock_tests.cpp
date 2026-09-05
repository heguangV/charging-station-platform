#include "user_demo_service.h"

#include <QCoreApplication>
#include <QDebug>

namespace
{
int failures = 0;

void expect(bool condition, const char* message)
{
    if (!condition)
    {
        qCritical() << "FAILED:" << message;
        ++failures;
    }
}
} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    ncs::user::MockUserClientService service;
    QString message;

    expect(service.configureScenario(QStringLiteral("low-balance"), &message),
           "low-balance scenario must configure");
    expect(!service.reserve(1, QStringLiteral("ZGC-DC-01"), &message),
           "low-balance scenario must reject reservations");
    expect(message.contains(QStringLiteral("余额不足")), "low-balance error must be readable");

    expect(service.configureScenario(QStringLiteral("no-available-charger"), &message),
           "no-available-charger scenario must configure");
    for (const auto& charger : service.chargers(1))
        expect(charger.status != QStringLiteral("空闲"),
               "unavailable scenario must expose no idle charger");

    expect(service.configureScenario(QStringLiteral("active-charging"), &message),
           "active-charging scenario must configure");
    expect(service.hasUnfinishedOrder(), "active scenario must expose unfinished order");
    expect(service.progress().durationSeconds > 0, "active scenario must expose charge progress");

    expect(service.configureScenario(QStringLiteral("happy-path"), &message),
           "happy-path scenario must configure");
    expect(service.reserve(1, QStringLiteral("ZGC-DC-01"), &message),
           "happy-path must reserve an idle charger");
    for (int second = 0; second < 15 * 60; ++second)
        service.tick();
    expect(!service.hasUnfinishedOrder(), "expired reservation must not remain active");
    expect(!service.orders().isEmpty() &&
               service.orders().first().status == QStringLiteral("已超时"),
           "expired reservation must update order status");

    if (failures == 0)
        qInfo() << "All user mock tests passed.";
    return failures == 0 ? 0 : 1;
}
