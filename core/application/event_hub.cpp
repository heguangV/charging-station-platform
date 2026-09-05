#include "core/application/event_hub.h"

#include <algorithm>
#include <cstdio>

namespace ncs::core::application {
namespace {

constexpr std::string_view kPingFrame = R"({"type":"ping"})";
constexpr std::string_view kProgressType = "charge.progress";

struct PendingClose {
    std::shared_ptr<WebSocketPeer> peer;
    std::uint16_t code;
    std::string reason;
};

std::int64_t unixNow()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

EventHub::EventHub(EventHubOptions options)
    : options_(std::move(options))
{
}

EventHub::~EventHub()
{
    shutdown();
}

bool EventHub::registerPeer(const std::shared_ptr<WebSocketPeer> peer, PeerInfo info)
{
    if (!peer || info.sessionId <= 0) return false;
    std::shared_ptr<WebSocketPeer> replaced;
    {
        std::lock_guard lock(mutex_);
        if (shuttingDown_) return false;
        const auto duplicate = std::find_if(peers_.begin(), peers_.end(),
            [&info](const auto &entry) { return entry->info.sessionId == info.sessionId; });
        if (duplicate != peers_.end()) {
            // A newer connection for the same session takes over the old one
            // (reconnect-after-drop). The old peer is closed outside the
            // lock; its onclose unregisters by identity and cannot remove
            // the replacement.
            replaced = (*duplicate)->peer;
            peers_.erase(duplicate);
        }
        if (peers_.size() >= options_.maxPeers) return false;

        auto entry = std::make_unique<PeerEntry>();
        entry->peer = std::move(peer);
        entry->info = std::move(info);
        if (entry->info.tokenKind == TokenKind::User) {
            const std::string &principal = entry->info.principalId;
            if (principal.rfind("user:", 0) == 0) {
                entry->userId = std::stoll(principal.substr(5));
            }
        }
        peers_.push_back(std::move(entry));
    }
    if (replaced) {
        replaced->close(1001, "replaced by a newer connection");
    }
    return true;
}

void EventHub::unregisterPeer(const std::int64_t sessionId,
                              const WebSocketPeer *identity)
{
    std::lock_guard lock(mutex_);
    unregisterLocked(sessionId, identity);
}

bool EventHub::atPeerCapacity() const
{
    std::lock_guard lock(mutex_);
    return peers_.size() >= options_.maxPeers;
}

bool EventHub::canAcceptPeer(const std::int64_t sessionId) const
{
    std::lock_guard lock(mutex_);
    if (shuttingDown_) return false;
    return peers_.size() < options_.maxPeers
        || std::any_of(peers_.begin(), peers_.end(), [sessionId](const auto &entry) {
               return entry->info.sessionId == sessionId;
           });
}

bool EventHub::publish(
    std::string type,
    std::string dataJson,
    EventScope scope,
    std::int64_t occurredAt)
{
    if (occurredAt <= 0) occurredAt = nowSec();
    std::vector<PendingClose> closers;
    {
        std::lock_guard lock(mutex_);
        if (shuttingDown_) return false;

        ++sequence_;
        // eventId reflects allocation/delivery time. occurredAt intentionally
        // remains the original business/outbox time and may be days older.
        const std::int64_t allocatedAt = nowSec();
        const std::string eventId = nextEventIdLocked(allocatedAt);
        const std::string frame = "{\"type\":\"" + escapeJson(type) + "\",\"eventId\":\""
            + eventId + "\",\"sequence\":" + std::to_string(sequence_)
            + ",\"occurredAt\":" + std::to_string(occurredAt)
            + ",\"data\":" + dataJson + "}";

        for (auto iterator = peers_.begin(); iterator != peers_.end();) {
            auto &entry = *iterator;
            const bool matchesSession = scope.sessionId
                && entry->info.sessionId == *scope.sessionId;
            const bool matchesUser = scope.userId && entry->userId
                && *entry->userId == *scope.userId;
            const bool matchesAdmins = scope.admins
                && entry->info.tokenKind == TokenKind::Administrator;
            const bool matchesDashboards = scope.dashboards
                && entry->info.tokenKind == TokenKind::Dashboard;
            if (!(matchesSession || matchesUser || matchesAdmins || matchesDashboards)) {
                ++iterator;
                continue;
            }
            const EnqueueOutcome outcome = enqueueLocked(*entry, type, frame,
                                                         allocatedAt);
            if (outcome == EnqueueOutcome::Closed) {
                closers.push_back(PendingClose{entry->peer, 1013, "buffer overflow"});
                iterator = peers_.erase(iterator);
                continue;
            }
            ++iterator;
        }
    }
    // Overflow closers run outside the lock: Crow's close() dispatches inline
    // on io threads and its close handler re-enters this hub.
    for (const auto &closer : closers) {
        closer.peer->close(closer.code, closer.reason);
    }
    return true;
}

void EventHub::recordPong(const std::int64_t sessionId, const std::int64_t nowSec,
                          const WebSocketPeer *identity)
{
    std::lock_guard lock(mutex_);
    const auto found = std::find_if(peers_.begin(), peers_.end(),
        [sessionId](const auto &entry) { return entry->info.sessionId == sessionId; });
    if (found == peers_.end()) return;
    // A replaced connection's late pong must not refresh the new peer.
    if (identity && (*found)->peer.get() != identity) return;
    (*found)->lastPongAt = nowSec;
}

void EventHub::tickHeartbeat(const std::int64_t nowSec)
{
    std::vector<std::pair<std::int64_t, std::string>> livenessTargets;
    std::vector<PendingClose> closers;
    {
        std::lock_guard lock(mutex_);
        for (auto iterator = peers_.begin(); iterator != peers_.end();) {
            auto &entry = *iterator;
            if (entry->closing) {
                ++iterator;
                continue;
            }
            if (entry->lastPongAt == 0) entry->lastPongAt = nowSec;
            if (nowSec - entry->lastPongAt >= options_.pongTimeout.count()) {
                closers.push_back(PendingClose{entry->peer, 4000, "heartbeat timeout"});
                iterator = peers_.erase(iterator);
                continue;
            }
            const bool dueForPing = nowSec - entry->lastPongAt
                    >= options_.pingInterval.count()
                && nowSec - entry->lastPingAt >= options_.pingInterval.count();
            if (dueForPing) {
                entry->lastPingAt = nowSec;
                entry->peer->sendText(std::string(kPingFrame));
                if (options_.livenessCheck) {
                    livenessTargets.emplace_back(entry->info.sessionId, entry->info.token);
                }
            }
            ++iterator;
        }
    }
    for (const auto &closer : closers) {
        closer.peer->close(closer.code, closer.reason);
    }
    if (!options_.livenessCheck) return;

    std::vector<std::int64_t> staleSessions;
    for (const auto &[sessionId, token] : livenessTargets) {
        try {
            if (!options_.livenessCheck(token)) staleSessions.push_back(sessionId);
        } catch (...) {
            // An observer failure is treated as liveness failure.
            staleSessions.push_back(sessionId);
        }
    }
    if (staleSessions.empty()) return;
    std::vector<PendingClose> staleClosers;
    {
        std::lock_guard lock(mutex_);
        for (const auto sessionId : staleSessions) {
            const auto found = std::find_if(peers_.begin(), peers_.end(),
                [sessionId](const auto &entry) { return entry->info.sessionId == sessionId; });
            if (found == peers_.end()) continue;
            staleClosers.push_back(PendingClose{(*found)->peer, 4002, "session expired"});
            peers_.erase(found);
        }
    }
    for (const auto &closer : staleClosers) {
        closer.peer->close(closer.code, closer.reason);
    }
}

void EventHub::notifySessionRevoked(
    const std::int64_t sessionId,
    const std::string_view /*principalId*/)
{
    const std::int64_t nowSec = this->nowSec();
    std::shared_ptr<WebSocketPeer> peer;
    std::string frame;
    {
        std::lock_guard lock(mutex_);
        const auto found = std::find_if(peers_.begin(), peers_.end(),
            [sessionId](const auto &entry) { return entry->info.sessionId == sessionId; });
        if (found == peers_.end()) return;
        peer = (*found)->peer;
        peers_.erase(found);

        ++sequence_;
        const std::string eventId = nextEventIdLocked(nowSec);
        frame = "{\"type\":\"session.revoked\",\"eventId\":\""
            + eventId + "\",\"sequence\":" + std::to_string(sequence_)
            + ",\"occurredAt\":" + std::to_string(nowSec)
            + ",\"data\":{\"reason\":\"revoked\"}}";
    }
    // The peer is already out of the registry, so no concurrent publisher
    // can interleave; frame-then-close order is preserved on the wire.
    peer->sendText(std::move(frame));
    peer->close(4001, "session revoked");
}

std::string EventHub::nextEventIdLocked(const std::int64_t nowSec)
{
    const std::int64_t day = nowSec / 86400;
    // Do not move the allocation day backwards if the system clock is
    // corrected after midnight; that would reissue an earlier day's IDs.
    if (eventDay_ < 0 || day > eventDay_) {
        eventDay_ = static_cast<std::int32_t>(day);
        eventSerial_ = 0;
    }
    return eventIdFor(static_cast<std::int64_t>(eventDay_) * 86400,
                      ++eventSerial_);
}

std::int64_t EventHub::nowSec() const
{
    return options_.clock ? options_.clock() : unixNow();
}

void EventHub::closeSession(
    const std::int64_t sessionId,
    const std::uint16_t code,
    std::string reason)
{
    std::shared_ptr<WebSocketPeer> peer;
    {
        std::lock_guard lock(mutex_);
        const auto found = std::find_if(peers_.begin(), peers_.end(),
            [sessionId](const auto &entry) { return entry->info.sessionId == sessionId; });
        if (found == peers_.end()) return;
        peer = (*found)->peer;
        peers_.erase(found);
    }
    peer->close(code, std::move(reason));
}

std::vector<std::int64_t> EventHub::snapshotUserPeerIds() const
{
    std::lock_guard lock(mutex_);
    std::vector<std::int64_t> ids;
    for (const auto &entry : peers_) {
        if (entry->userId) ids.push_back(*entry->userId);
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

std::uint64_t EventHub::currentSequence() const
{
    std::lock_guard lock(mutex_);
    return sequence_;
}

std::size_t EventHub::peerCount() const
{
    std::lock_guard lock(mutex_);
    return peers_.size();
}

void EventHub::shutdown()
{
    std::lock_guard lock(mutex_);
    shuttingDown_ = true;
    peers_.clear();
}

std::string EventHub::escapeJson(const std::string_view value)
{
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        switch (character) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (static_cast<unsigned char>(character) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", character);
                result += buffer;
            } else {
                result += character;
            }
        }
    }
    return result;
}

void EventHub::civilFromDays(
    const std::int64_t daysSinceEpoch,
    std::int32_t &year,
    std::int32_t &month,
    std::int32_t &day)
{
    const std::int64_t z = daysSinceEpoch + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t doe = z - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y = yoe + era * 400;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    day = static_cast<std::int32_t>(doy - (153 * mp + 2) / 5 + 1);
    month = static_cast<std::int32_t>(mp + (mp < 10 ? 3 : -9));
    year = static_cast<std::int32_t>(y + (month <= 2 ? 1 : 0));
}

std::string EventHub::eventIdFor(const std::int64_t nowSec,
                                 const std::uint64_t serial)
{
    const std::int64_t days = nowSec / 86400 - (nowSec % 86400 < 0 ? 1 : 0);
    std::int32_t year = 0;
    std::int32_t month = 0;
    std::int32_t day = 0;
    civilFromDays(days, year, month, day);
    char buffer[32];
    // Eight digits is the minimum width, not a modulo: larger serials expand
    // so the process-wide uniqueness guarantee is not reintroduced at 10^8.
    std::snprintf(buffer, sizeof(buffer), "EV%04d%02d%02d%08llu",
        year, month, day, static_cast<unsigned long long>(serial));
    return buffer;
}

void EventHub::advanceWindowLocked(PeerEntry &entry, const std::int64_t nowSec)
{
    const std::int64_t horizon = nowSec - options_.windowDuration.count();
    while (!entry.window.empty() && entry.window.front().atSec <= horizon) {
        entry.windowFrames -= 1;
        entry.windowBytes -= entry.window.front().bytes;
        entry.window.pop_front();
    }
}

EventHub::EnqueueOutcome EventHub::enqueueLocked(
    PeerEntry &entry,
    std::string type,
    std::string frame,
    const std::int64_t nowSec)
{
    if (entry.closing) return EnqueueOutcome::Closed;
    advanceWindowLocked(entry, nowSec);
    const std::size_t frameBytes = frame.size();
    const bool progress = type == kProgressType;
    if (entry.windowFrames + 1 > options_.maxWindowFrames
        || entry.windowBytes + frameBytes > options_.maxWindowBytes) {
        // Free capacity by discarding already-accounted progress frames first.
        while (!entry.window.empty() && entry.window.front().progress) {
            entry.windowFrames -= 1;
            entry.windowBytes -= entry.window.front().bytes;
            entry.window.pop_front();
        }
    }
    if (entry.windowFrames + 1 > options_.maxWindowFrames
        || entry.windowBytes + frameBytes > options_.maxWindowBytes) {
        if (progress) return EnqueueOutcome::Dropped;
        // The caller collects this peer and closes it after releasing the
        // hub lock (close() may re-enter the hub via the onclose callback).
        entry.closing = true;
        return EnqueueOutcome::Closed;
    }
    entry.peer->sendText(std::move(frame));
    entry.window.push_back(WindowEntry{nowSec, frameBytes, progress});
    entry.windowFrames += 1;
    entry.windowBytes += frameBytes;
    return EnqueueOutcome::Sent;
}

void EventHub::unregisterLocked(const std::int64_t sessionId,
                                const WebSocketPeer *identity)
{
    const auto found = std::find_if(peers_.begin(), peers_.end(),
        [sessionId](const auto &entry) { return entry->info.sessionId == sessionId; });
    if (found == peers_.end()) return;
    if (identity && (*found)->peer.get() != identity) return;
    peers_.erase(found);
}

} // namespace ncs::core::application
