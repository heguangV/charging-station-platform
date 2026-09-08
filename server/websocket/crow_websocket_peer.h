// WebSocket 传输适配器：把 crow::websocket::connection 适配为 EventHub 的 WebSocketPeer
// 接口（sendText/close）。 连接指针自 onopen 起有效、至 onclose 注销；close 可能同步重入 onclose
// 故用递归锁，已 detach 后的发送由 Crow 的弱锚点丢弃。
#pragma once

#include "core/application/event_hub.h"

#include <mutex>

namespace crow
{
namespace websocket
{
class connection;
} // namespace websocket
} // namespace crow

namespace ncs::server::websocket
{

// Adapts a crow::websocket::connection to the hub's transport interface. The
// raw pointer stays valid from the rule's onopen until the hub unregisters
// the peer in onclose; in-flight sends posted before destruction are dropped
// by crow's weak anchor.
class CrowWebSocketPeer final : public core::application::WebSocketPeer
{
  public:
    explicit CrowWebSocketPeer(crow::websocket::connection* connection);

    void sendText(std::string frame) override;
    void close(std::uint16_t code, std::string reason) override;
    void detach(const crow::websocket::connection* connection)
    {
        std::lock_guard lock(mutex_);
        if (connection_ == connection)
            connection_ = nullptr;
    }

  private:
    // close() may synchronously re-enter onclose on Crow's I/O thread.
    std::recursive_mutex mutex_;
    crow::websocket::connection* connection_ = nullptr;
};

} // namespace ncs::server::websocket
