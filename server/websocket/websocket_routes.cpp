#include "server/websocket/websocket_routes.h"

#include "server/controller/api_response.h"
#include "server/websocket/crow_websocket_peer.h"

#include <crow/websocket.h>

#include <chrono>

namespace ncs::server::websocket
{
namespace
{

constexpr std::string_view kPongFrame = R"({"type":"pong"})";

std::int64_t unixNow()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

struct WsHandshakeInfo
{
    core::application::AuthContext auth;
    std::string token;
    std::shared_ptr<CrowWebSocketPeer> peer; // set in onopen, used for
                                             // identity-checked callbacks
};

void closeWith(crow::websocket::connection& connection, const std::uint16_t code,
               const std::string reason)
{
    connection.close(reason, code);
}

} // namespace

// WebSocket 握手鉴权：解析并认证 Bearer 令牌；ML 任务令牌明确拒绝（Forbidden），缺失/
// 过期/吊销返回未接受，成功时返回认证上下文与原始令牌供升级完成后二次校验。
HandshakeResult authorizeHandshake(const crow::request& request,
                                   core::application::SessionManager& sessions,
                                   const std::chrono::system_clock::time_point now)
{
    HandshakeResult result;
    const auto bearer =
        core::application::SessionManager::parseBearer(request.get_header_value("Authorization"));
    if (!bearer)
        return result;
    result.token = std::string(*bearer);
    const auto context = sessions.authenticate(*bearer, now);
    if (!context)
        return result;
    if (context->tokenKind == core::application::TokenKind::MlTask)
    {
        result.decision = HandshakeDecision::Forbidden;
        result.auth = context;
        return result;
    }
    result.decision = HandshakeDecision::Accepted;
    result.auth = context;
    return result;
}

WebsocketRoutes::WebsocketRoutes(ServerApp& application,
                                 core::application::SessionManager& sessions,
                                 const std::shared_ptr<core::application::EventHub> hub,
                                 const std::size_t maxPayloadBytes)
{
    using core::application::EventScope;
    using core::application::PeerInfo;

    application.route_dynamic("/api/v1/events")
        .methods(crow::HTTPMethod::GET)
        .websocket(&application)
        .max_payload(maxPayloadBytes)
        // onaccept：HTTP 升级前鉴权；拒绝 MlTask 令牌与未认证连接，连接数达到上限时以
        // 403 直接拒绝（连接不升级），认证信息写入 userdata 供后续回调使用。
        .onaccept(
            [&sessions, hub](const crow::request& request, std::optional<crow::response>& response,
                             void** userdata)
            {
                const auto result =
                    authorizeHandshake(request, sessions, std::chrono::system_clock::now());
                if (result.decision == HandshakeDecision::Forbidden)
                {
                    response = ncs::server::controller::errorResponse(
                        core::domain::ErrorCode::Forbidden,
                        "ml task tokens cannot connect to the event stream", "");
                    return;
                }
                if (result.decision != HandshakeDecision::Accepted)
                {
                    response = ncs::server::controller::errorResponse(
                        core::domain::ErrorCode::Unauthorized,
                        "bearer token is missing, expired or revoked", "");
                    return;
                }
                if (!hub->canAcceptPeer(result.auth->sessionId))
                {
                    // 13.1: the connection limit is reported as a plain HTTP 403
                    // and the connection is never upgraded.
                    response = ncs::server::controller::errorResponse(
                        core::domain::ErrorCode::Forbidden, "websocket connection limit reached",
                        "");
                    return;
                }
                auto* info = new WsHandshakeInfo{*result.auth, result.token, nullptr};
                *userdata = info;
            })
        // onopen：升级完成回调——注册 peer 到事件中心；与容量竞争冲突时以 1013 关闭，
        // 随后广播 session.ready 通知客户端链路就绪。
        .onopen(
            [hub, &sessions](crow::websocket::connection& connection)
            {
                auto* info = static_cast<WsHandshakeInfo*>(connection.userdata());
                if (!info)
                    return;
                // Close the small gap between HTTP accept and WebSocket upgrade:
                // a token revoked during that interval must not become a peer.
                if (!sessions.authenticate(info->token, std::chrono::system_clock::now()))
                {
                    closeWith(connection, 4002, "session expired");
                    return;
                }
                info->peer = std::make_shared<CrowWebSocketPeer>(&connection);
                PeerInfo peerInfo;
                peerInfo.sessionId = info->auth.sessionId;
                peerInfo.principalId = info->auth.principalId;
                peerInfo.tokenKind = info->auth.tokenKind;
                peerInfo.token = info->token;
                if (!hub->registerPeer(info->peer, peerInfo))
                {
                    // Only reachable through a capacity race with another
                    // handshake (or shutdown); the duplicate-session case is
                    // handled by replacement inside the hub.
                    closeWith(connection, 1013, "server at capacity");
                    return;
                }
                hub->publish("session.ready", "{}",
                             EventScope{std::nullopt, peerInfo.sessionId, false, false}, unixNow());
            })
        // onmessage：仅识别文本 pong 心跳帧并刷新对端存活时间，其余消息一律忽略。
        .onmessage(
            [hub](crow::websocket::connection& connection, const std::string& message,
                  const bool isBinary)
            {
                if (isBinary || message != kPongFrame)
                    return;
                const auto* info = static_cast<WsHandshakeInfo*>(connection.userdata());
                if (!info || !info->peer)
                    return;
                hub->recordPong(info->auth.sessionId, unixNow(), info->peer.get());
            })
        // onclose：从事件中心注销 peer、解除连接关联并释放 userdata，防止连接与内存泄漏。
        .onclose(
            [hub](crow::websocket::connection& connection, const std::string&, const std::uint16_t)
            {
                auto* info = static_cast<WsHandshakeInfo*>(connection.userdata());
                if (!info)
                    return;
                if (info->peer)
                {
                    info->peer->detach(&connection);
                    hub->unregisterPeer(info->auth.sessionId, info->peer.get());
                }
                connection.userdata(nullptr);
                delete info;
            })
        .onerror(
            [](crow::websocket::connection&, const std::string&)
            {
                // onclose always follows and performs the registry cleanup.
            });
}

} // namespace ncs::server::websocket
