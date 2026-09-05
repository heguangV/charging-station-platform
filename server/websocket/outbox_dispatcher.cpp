#include "server/websocket/outbox_dispatcher.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <charconv>
#include <optional>
#include <string>
#include <system_error>

namespace ncs::server::websocket {
namespace {

using core::application::ChargingRepository;
using core::application::EventScope;
using core::application::OutboxEvent;

std::string compact(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
}

std::int64_t unixSeconds(const std::chrono::system_clock::time_point now)
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch())
        .count();
}

} // namespace

OutboxDispatcher::OutboxDispatcher(
    ChargingRepository &repository,
    const std::shared_ptr<core::application::EventHub> hub)
    : repository_(repository)
    , hub_(std::move(hub))
{
}

std::size_t OutboxDispatcher::dispatchOnce(
    const std::chrono::system_clock::time_point now)
{
    const std::int64_t nowSeconds = unixSeconds(now);
    const auto batch = repository_.pollOutbox(nowSeconds, kBatchLimit);
    if (batch.empty()) return 0;

    std::vector<std::int64_t> deliveredIds;
    std::vector<std::int64_t> deadIds;
    for (const auto &row : batch) {
        try {
            if (publishedThisRun_.count(row.id)) {
                // Already delivered earlier this process run; only the
                // database mark is outstanding. Never republish.
                deliveredIds.push_back(row.id);
                continue;
            }
            if (row.eventType == "order.settled") {
                const auto flow = repository_.flow(row.aggregateId);
                const auto order = flow
                    ? repository_.orderByFlow(row.aggregateId)
                    : std::nullopt;
                if (!flow || !order) {
                    // The row and its order are written in one transaction,
                    // so this is inconsistent data. Drain instead of
                    // publishing a truncated frame (13.3 requires the
                    // settlement fields).
                    deadIds.push_back(row.id);
                    continue;
                }
                QJsonObject data;
                data["flowNo"] = QString::fromStdString(row.aggregateId);
                data["orderNo"] = QString::fromStdString(order->orderNo);
                data["amountCent"] = static_cast<qint64>(order->amountCent);
                data["energyMwh"] = static_cast<qint64>(order->energyMwh);
                data["settledAt"] = static_cast<qint64>(order->settledAt.value_or(0));
                data["status"] = order->status;
                if (hub_->publish(
                        "order.settled", compact(data),
                        EventScope{flow->userId, std::nullopt, true, false},
                        row.createdAt)) {
                    deliveredIds.push_back(row.id);
                    publishedThisRun_.insert(row.id);
                }
            } else if (row.eventType == "flow.updated") {
                const auto flow = repository_.flow(row.aggregateId);
                if (!flow) {
                    deadIds.push_back(row.id);
                    continue;
                }
                QJsonObject data;
                data["flowNo"] = QString::fromStdString(row.aggregateId);
                data["fromStatus"] = row.fromStatus;
                data["toStatus"] = row.toStatus;
                data["statusText"] = QString::fromStdString(
                    core::application::flowStatusText(row.toStatus));
                data["reasonCode"] = QString::fromStdString(row.reasonCode);
                data["stationId"] = static_cast<qint64>(flow->stationId);
                if (const auto station = repository_.station(flow->stationId)) {
                    data["stationName"] = QString::fromStdString(station->name);
                }
                if (flow->chargerCode) {
                    data["chargerCode"] =
                        QString::fromStdString(*flow->chargerCode);
                }
                if (flow->status == static_cast<int>(core::application::FlowStatus::Queued)) {
                    const auto queue = repository_.queue(
                        flow->stationId, flow->chargerType);
                    for (std::size_t position = 0; position < queue.size();
                         ++position) {
                        if (queue[position] == row.aggregateId) {
                            data["queuePosition"] =
                                static_cast<qint64>(position + 1);
                            break;
                        }
                    }
                }
                if (hub_->publish(
                        "flow.updated", compact(data),
                        EventScope{flow->userId, std::nullopt, true, false},
                        row.createdAt)) {
                    deliveredIds.push_back(row.id);
                    publishedThisRun_.insert(row.id);
                }
            } else if (row.eventType == "charger.statusChanged") {
                std::int64_t chargerId = 0;
                const char *const first = row.aggregateId.data();
                const char *const last = first + row.aggregateId.size();
                const auto parsed = std::from_chars(first, last, chargerId);
                if (parsed.ec != std::errc{} || parsed.ptr != last ||
                    chargerId <= 0) {
                    // A malformed aggregate id can never be enriched; drain
                    // it instead of burning its retry budget every tick.
                    deadIds.push_back(row.id);
                    continue;
                }
                QJsonObject data;
                data["chargerId"] = static_cast<qint64>(chargerId);
                data["fromStatus"] = row.fromStatus;
                data["toStatus"] = row.toStatus;
                data["reason"] = QString::fromStdString(row.reasonCode);
                if (const auto charger = repository_.charger(chargerId)) {
                    data["chargerCode"] = QString::fromStdString(charger->code);
                    data["stationId"] = static_cast<qint64>(charger->stationId);
                }
                if (hub_->publish(
                        "charger.statusChanged", compact(data),
                        EventScope{std::nullopt, std::nullopt, true, false},
                        row.createdAt)) {
                    deliveredIds.push_back(row.id);
                    publishedThisRun_.insert(row.id);
                }
            } else {
                // Unknown event type: drain so a poisoned row cannot wedge
                // the dispatcher.
                deadIds.push_back(row.id);
            }
        } catch (...) {
            // A malformed/temporarily unreadable row must consume only its
            // own retry budget; later rows in the same poll were not tried.
            if (!publishedThisRun_.count(row.id)) {
                try {
                    repository_.markOutboxAttempted({row.id});
                } catch (...) {
                    // The next dispatcher tick will retry the still-pending
                    // row; never unwind into the runtime timer.
                }
            }
        }
    }
    try {
        if (!deadIds.empty()) {
            repository_.markOutboxDead(deadIds);
        }
        if (!deliveredIds.empty()) {
            repository_.markOutboxDelivered(deliveredIds);
        }
        for (const auto id : deliveredIds) {
            publishedThisRun_.erase(id);
        }
    } catch (...) {
        // Status persistence failed. Published rows remain in the process
        // cache and will be marked, rather than republished, next tick.
        return 0;
    }

    if (!deliveredIds.empty()
        && now - lastRefreshAt_ >= kRefreshThrottle) {
        lastRefreshAt_ = now;
        hub_->publish("dashboard.refresh", "{}",
                      EventScope{std::nullopt, std::nullopt, false, true},
                      nowSeconds);
    }
    return deliveredIds.size();
}

} // namespace ncs::server::websocket
