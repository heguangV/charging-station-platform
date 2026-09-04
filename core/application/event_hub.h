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

namespace ncs::core::application {

// Transport abstraction for one accepted WebSocket connection. Implementations
// are allowed to be called from any thread.
class WebSocketPeer {
public:
    virtual ~WebSocketPeer() = default;
    // Delivers one fully serialized event frame (fire-and-forget).
    virtual void sendText(std::string frame) = 0;
    virtual void close(std::uint16_t code, std::string reason) = 0;
};

struct PeerInfo {
    std::int64_t sessionId = 0;
    std::string principalId;                // "user:12" / "admin:2" / "dashboard:..."
    TokenKind tokenKind = TokenKind::User;  // MlTask peers are never registered
    std::string token;                      // raw bearer, memory-only, for liveness re-auth
};

struct EventScope {
    std::optional<std::int64_t> userId;     // user peers of this user
    std::optional<std::int64_t> sessionId;  // one exact session, any token kind
    bool admins = false;
    bool dashboards = false;
};

struct EventHubOptions {
    std::size_t maxPeers = 100;             // NFR-P-05
    std::size_t maxWindowFrames = 256;      // frames admitted to the transport
    std::size_t maxWindowBytes = 1 << 20;   // per peer within one window
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
class EventHub final {
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
    void unregisterPeer(std::int64_t sessionId, const WebSocketPeer *identity);
    bool atPeerCapacity() const;
    // Capacity preflight that preserves the documented same-session
    // replacement behavior even when the peer table is otherwise full.
    bool canAcceptPeer(std::int64_t sessionId) const;

    // Allocates one global sequence and eventId and routes the frame to every
    // matching peer. dataJson must be valid JSON text. Returns false while
    // shutting down (nothing is delivered).
    bool publish(std::string type, std::string dataJson, EventScope scope,
                 std::int64_t occurredAt);

    void recordPong(std::int64_t sessionId, std::int64_t nowSec,
                    const WebSocketPeer *identity);
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
    void closeSession(std::int64_t sessionId, std::uint16_t code,
                      std::string reason);

    // Unique, sorted user ids of connected user peers.
    std::vector<std::int64_t> snapshotUserPeerIds() const;
    std::uint64_t currentSequence() const;
    std::size_t peerCount() const;

    // Drops the peer registry without sending anything (io contexts are
    // already stopped at that point).
    void shutdown();

private:
    struct WindowEntry {
        std::int64_t atSec = 0;
        std::size_t bytes = 0;
        bool progress = false;
    };

    struct PeerEntry {
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

    enum class EnqueueOutcome { Sent, Dropped, Closed };

    static std::string escapeJson(std::string_view value);
    static std::string eventIdFor(std::int64_t nowSec, std::uint64_t serial);
    static void civilFromDays(std::int64_t daysSinceEpoch, std::int32_t &year,
                              std::int32_t &month, std::int32_t &day);

    void advanceWindowLocked(PeerEntry &entry, std::int64_t nowSec);
    std::int64_t nowSec() const;
    std::string nextEventIdLocked(std::int64_t nowSec);
    EnqueueOutcome enqueueLocked(PeerEntry &entry, std::string type,
                                 std::string frame, std::int64_t nowSec);
    void unregisterLocked(std::int64_t sessionId, const WebSocketPeer *identity);

    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<PeerEntry>> peers_;
    std::uint64_t sequence_ = 0;
    std::int32_t eventDay_ = -1;
    std::uint64_t eventSerial_ = 0;
    bool shuttingDown_ = false;
    EventHubOptions options_;
};

} // namespace ncs::core::application
