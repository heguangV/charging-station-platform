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
