// 充电进度推送器：约每秒一轮，向每个拥有进行中充电流程的在线用户 peer 推送 charge.progress
// 帧（契约 13.4）。 只在阻塞线程池运行；满容量 100 连接时每秒至多 100 次轻量 SQLite 读。
#pragma once

#include "core/application/charge_flow_service.h"
#include "core/application/event_hub.h"

#include <chrono>
#include <memory>

namespace ncs::server::websocket
{

// Pushes charge.progress frames (about once per second) to every connected
// user peer that owns a charging flow. Runs only on a blocking-executor
// worker; at the 100-peer capacity this is at most 100 light SQLite reads
// per second.
class ChargeProgressPusher final
{
  public:
    ChargeProgressPusher(core::application::ChargeFlowService& flows,
                         std::shared_ptr<core::application::EventHub> hub);

    void pushOnce(std::chrono::system_clock::time_point now);

  private:
    core::application::ChargeFlowService& flows_;
    std::shared_ptr<core::application::EventHub> hub_;
};

} // namespace ncs::server::websocket
