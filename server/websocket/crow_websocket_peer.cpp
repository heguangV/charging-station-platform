#include "server/websocket/crow_websocket_peer.h"

#include <crow/websocket.h>

namespace ncs::server::websocket {

CrowWebSocketPeer::CrowWebSocketPeer(crow::websocket::connection *connection)
    : connection_(connection)
{
}

void CrowWebSocketPeer::sendText(std::string frame)
{
    std::lock_guard lock(mutex_);
    if (connection_) {
        connection_->send_text(std::move(frame));
    }
}

void CrowWebSocketPeer::close(const std::uint16_t code, std::string reason)
{
    std::lock_guard lock(mutex_);
    if (connection_) {
        connection_->close(std::move(reason), code);
    }
}

void CrowWebSocketPeer::detach(const crow::websocket::connection *connection)
{
    std::lock_guard lock(mutex_);
    if (connection_ == connection) connection_ = nullptr;
}

} // namespace ncs::server::websocket
