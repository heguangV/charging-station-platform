#pragma once

#include "core/application/charge_flow_service.h"
#include "core/application/event_hub.h"

#include <chrono>
#include <memory>

namespace ncs::server::websocket {

// Pushes charge.progress frames (about once per second) to every connected
// user peer that owns a charging flow. Runs only on a blocking-executor
// worker; at the 100-peer capacity this is at most 100 light SQLite reads
// per second.
class ChargeProgressPusher final {
public:
    ChargeProgressPusher(core::application::ChargeFlowService &flows,
                         std::shared_ptr<core::application::EventHub> hub);

    void pushOnce(std::chrono::system_clock::time_point now);

private:
    core::application::ChargeFlowService &flows_;
    std::shared_ptr<core::application::EventHub> hub_;
};

} // namespace ncs::server::websocket
