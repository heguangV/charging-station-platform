#include "server/websocket/progress_pusher.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace ncs::server::websocket {
namespace {

std::string compact(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
}

} // namespace

ChargeProgressPusher::ChargeProgressPusher(
    core::application::ChargeFlowService &flows,
    const std::shared_ptr<core::application::EventHub> hub)
    : flows_(flows)
    , hub_(std::move(hub))
{
}

void ChargeProgressPusher::pushOnce(
    const std::chrono::system_clock::time_point now)
{
    for (const auto userId : hub_->snapshotUserPeerIds()) {
        const auto active = flows_.activeFlow(userId, now);
        if (!active.hasActiveFlow || !active.flow
            || active.flow->status
                != static_cast<int>(core::application::FlowStatus::Charging)) {
            continue;
        }
        const auto result =
            flows_.progress(userId, active.flow->flowNo, now);
        if (!result.ok() || !result.value) continue;
        const auto &progress = *result.value;
        QJsonObject data;
        data["flowNo"] = QString::fromStdString(progress.flowNo);
        data["orderNo"] = QString::fromStdString(progress.orderNo);
        data["status"] = progress.status;
        data["statusText"] = QString::fromStdString(progress.statusText);
        data["durationSec"] = static_cast<qint64>(progress.durationSec);
        data["energyMwh"] = static_cast<qint64>(progress.energyMwh);
        data["amountCent"] = static_cast<qint64>(progress.amountCent);
        data["powerWatt"] = static_cast<qint64>(progress.powerWatt);
        data["simulatedSoc"] = static_cast<qint64>(progress.simulatedSoc);
        data["calculatedAt"] = static_cast<qint64>(progress.calculatedAt);
        hub_->publish("charge.progress", compact(data),
                      core::application::EventScope{
                          userId, std::nullopt, false, false},
                      progress.calculatedAt);
    }
}

} // namespace ncs::server::websocket
