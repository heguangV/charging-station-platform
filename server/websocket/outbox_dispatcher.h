#pragma once

#include "core/application/charging_repository.h"
#include "core/application/event_hub.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_set>

namespace ncs::server::websocket {

// Polls pending outbox rows, serializes the frozen WebSocket event payloads
// and publishes them through the hub. Runs only on a blocking-executor
// worker (SQLite is never touched on the crow event loop); the scheduler's
// in-flight guard keeps dispatchOnce single-threaded.
//
// Delivery and its database mark cannot be atomic. Rows published but not yet
// marked delivered are tracked in publishedThisRun_ so a failed mark is
// retried on a later tick WITHOUT republishing: no event is delivered twice
// within one process run (13.5), while a crash before the mark still redelivers
// after restart (also per 13.5).
class OutboxDispatcher final {
public:
    OutboxDispatcher(core::application::ChargingRepository &repository,
                     std::shared_ptr<core::application::EventHub> hub);

    // Returns the number of rows marked delivered in this batch.
    std::size_t dispatchOnce(std::chrono::system_clock::time_point now);

private:
    static constexpr int kBatchLimit = 256;
    static constexpr std::chrono::seconds kRefreshThrottle{5};

    core::application::ChargingRepository &repository_;
    std::shared_ptr<core::application::EventHub> hub_;
    std::chrono::system_clock::time_point lastRefreshAt_{};
    std::unordered_set<std::int64_t> publishedThisRun_;
};

} // namespace ncs::server::websocket
