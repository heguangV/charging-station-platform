// 进程内 WebSocket 事件广播枢纽：为事件分配全局单调序列与 eventId，按 EventScope
// 路由给指定用户/会话或全部管理员/大屏连接。 依赖 WebSocketPeer 传输抽象（由 Crow 连接适配），并与
// SessionManager 协作推送会话吊销通知（session.revoked，关闭码 4001）。
// 约束：线程安全且绝不持有其他锁时回调 peer；peer 满时拒绝注册（关闭码
// 1013）；带滑动窗口限流、心跳与可选令牌活性复查。

#pragma once

#include "core/application/session_manager.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ncs::core::application
{

// Transport abstraction for one accepted WebSocket connection. Implementations
// are allowed to be called from any thread.
class WebSocketPeer
{
  public:
    virtual ~WebSocketPeer() = default;
    // Delivers one fully serialized event frame (fire-and-forget).
    virtual void sendText(std::string frame) = 0;
    // 关闭底层连接并携带关闭码与原因（如 1013 容量满、4001 会话吊销、4002
    // 令牌失效）；实现可能从任意线程被调用。
    virtual void close(std::uint16_t code, std::string reason) = 0;
};

struct PeerInfo
{
    std::int64_t sessionId = 0;
    std::string principalId;               // "user:12" / "admin:2" / "dashboard:..."
    TokenKind tokenKind = TokenKind::User; // MlTask peers are never registered
    std::string token;                     // raw bearer, memory-only, for liveness re-auth
};

struct EventScope
{
    std::optional<std::int64_t> userId;    // user peers of this user
    std::optional<std::int64_t> sessionId; // one exact session, any token kind
    bool admins = false;
    bool dashboards = false;
};

struct EventHubOptions
{
    std::size_t maxPeers = 100;           // NFR-P-05
    std::size_t maxWindowFrames = 256;    // frames admitted to the transport
    std::size_t maxWindowBytes = 1 << 20; // per peer within one window
    std::chrono::seconds windowDuration{60};
    std::chrono::seconds pingInterval{30};
    std::chrono::seconds pongTimeout{60};
    // Optional; when set, every heartbeat re-validates the peer's token and
    // closes stale peers with code 4002.
    std::function<bool(std::string_view token)> livenessCheck;
    // Test seam for UTC event-id allocation; production leaves it empty.
    std::function<std::int64_t()> clock;
};

// Publishes monotonic event frames to connected peers. Thread-safe: every
// entry point takes the hub mutex; peers are never called while any other
// lock (in particular SessionManager's) is held.
class EventHub final
{
  public:
    explicit EventHub(EventHubOptions options = {});
    ~EventHub();

    // Returns false when shutting down or at peer capacity; the caller should
    // close the connection with 1013. A peer whose sessionId is already
    // registered replaces the old one (reconnect-after-drop): the old peer is
    // closed with 1001 outside the hub lock.
    bool registerPeer(std::shared_ptr<WebSocketPeer> peer, PeerInfo info);
    // Idempotent. When identity is given, removes the entry only if it still
    // belongs to that peer; a replaced connection's onclose therefore cannot
    // unregister its replacement.
    void unregisterPeer(std::int64_t sessionId, const WebSocketPeer* identity);
    bool atPeerCapacity() const;
    // Capacity preflight that preserves the documented same-session
    // replacement behavior even when the peer table is otherwise full.
    bool canAcceptPeer(std::int64_t sessionId) const;

    // Allocates one global sequence and eventId and routes the frame to every
    // matching peer. dataJson must be valid JSON text. Returns false while
    // shutting down (nothing is delivered).
    bool publish(std::string type, std::string dataJson, EventScope scope, std::int64_t occurredAt);

    // 记录心跳回包（仅当 identity 仍是该会话的当前连接时生效），供 tickHeartbeat 判定 pong 超时。
    void recordPong(std::int64_t sessionId, std::int64_t nowSec, const WebSocketPeer* identity);
    // Phase 1: pong timeouts and pings. Phase 2: optional liveness re-auth
    // (runs outside the hub lock). Peers are never closed while the hub
    // mutex is held: Crow's close() dispatches inline on io threads and can
    // re-enter this hub through the onclose callback.
    void tickHeartbeat(std::int64_t nowSec);

    // Sends a session.revoked frame to the peer of that exact session (if
    // connected) and closes it with 4001. Frame-then-close ordering is
    // preserved; both happen outside the hub lock.
    void notifySessionRevoked(std::int64_t sessionId, std::string_view principalId);

    // Closes the peer of that exact session (if connected) with the given
    // code without sending a frame. Used when the revocation notification
    // cannot be handed to the worker pool (overload fallback).
    void closeSession(std::int64_t sessionId, std::uint16_t code, std::string reason);

    // Unique, sorted user ids of connected user peers.
    std::vector<std::int64_t> snapshotUserPeerIds() const;
    std::uint64_t currentSequence() const;
    std::size_t peerCount() const;

    // Drops the peer registry without sending anything (io contexts are
    // already stopped at that point).
    void shutdown();

  private:
    struct WindowEntry
    {
        std::int64_t atSec = 0;
        std::size_t bytes = 0;
        bool progress = false;
    };

    struct PeerEntry
    {
        std::shared_ptr<WebSocketPeer> peer;
        PeerInfo info;
        std::optional<std::int64_t> userId;
        std::deque<WindowEntry> window;
        std::size_t windowFrames = 0;
        std::size_t windowBytes = 0;
        std::int64_t lastPongAt = 0;
        std::int64_t lastPingAt = 0;
        bool closing = false;
    };

    enum class EnqueueOutcome
    {
        Sent,
        Dropped,
        Closed
    };

    static std::string escapeJson(std::string_view value);
    static std::string eventIdFor(std::int64_t nowSec, std::uint64_t serial);
    static void civilFromDays(std::int64_t daysSinceEpoch, std::int32_t& year, std::int32_t& month,
                              std::int32_t& day);

    void advanceWindowLocked(PeerEntry& entry, std::int64_t nowSec);
    std::int64_t nowSec() const;
    std::string nextEventIdLocked(std::int64_t nowSec);
    EnqueueOutcome enqueueLocked(PeerEntry& entry, std::string type, std::string frame,
                                 std::int64_t nowSec);
    void unregisterLocked(std::int64_t sessionId, const WebSocketPeer* identity);

    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<PeerEntry>> peers_;
    std::uint64_t sequence_ = 0;
    std::int32_t eventDay_ = -1;
    std::uint64_t eventSerial_ = 0;
    bool shuttingDown_ = false;
    EventHubOptions options_;
};

} // namespace ncs::core::application
