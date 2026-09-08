// WebSocket 路由：注册 GET /api/v1/events 升级端点；authorizeHandshake 解析并认证令牌（拒绝 ML
// 任务令牌）后接入 EventHub。 升级握手绕过请求策略中间件（Crow
// 升级时丢弃其响应），因此鉴权、连接容量与最大帧长都在此自行把关。
#pragma once

#include "core/application/event_hub.h"
#include "core/application/session_manager.h"
#include "server/server_app.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

namespace ncs::server::websocket
{

enum class HandshakeDecision
{
    Accepted,
    Unauthorized,
    Forbidden
};

struct HandshakeResult
{
    HandshakeDecision decision = HandshakeDecision::Unauthorized;
    std::optional<core::application::AuthContext> auth;
    std::string token;
};

// Pure handshake decision: parse the Authorization header, authenticate the
// token, and reject ML task tokens (they are scoped to /internal/ml only).
// Unit-testable with a constructed crow::request.
HandshakeResult authorizeHandshake(const crow::request& request,
                                   core::application::SessionManager& sessions,
                                   std::chrono::system_clock::time_point now);

// Registers GET /api/v1/events as a WebSocket upgrade route. The handshake
// path bypasses the request middleware (crow discards its response on
// upgrade), so authentication and capacity limits live entirely here.
class WebsocketRoutes final
{
  public:
    WebsocketRoutes(ServerApp& application, core::application::SessionManager& sessions,
                    std::shared_ptr<core::application::EventHub> hub, std::size_t maxPayloadBytes);
};

} // namespace ncs::server::websocket
