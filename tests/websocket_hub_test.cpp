#include "core/application/event_hub.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace ncs::core::application;

class FakePeer final : public WebSocketPeer {
public:
    explicit FakePeer(const std::int64_t id)
        : id_(id)
    {
    }

    void sendText(std::string frame) override
    {
        events_.emplace_back(frame);
        ++sent_;
    }

    void close(const std::uint16_t code, std::string reason) override
    {
        closes_.emplace_back(code, std::move(reason));
        ++closed_;
    }

    const std::vector<std::string> &events() const { return events_; }
    const std::vector<std::pair<std::uint16_t, std::string>> &closes() const
    {
        return closes_;
    }
    std::size_t sent() const { return sent_; }
    std::size_t closed() const { return closed_; }
    std::int64_t id() const { return id_; }

private:
    std::int64_t id_;
    std::vector<std::string> events_;
    std::vector<std::pair<std::uint16_t, std::string>> closes_;
    std::size_t sent_ = 0;
    std::size_t closed_ = 0;
};

std::int64_t sequenceOf(const std::string &frame)
{
    constexpr std::string_view marker = "\"sequence\":";
    const auto position = frame.find(marker);
    if (position == std::string::npos) return -1;
    const auto start = position + marker.size();
    const auto end = frame.find(',', start);
    return std::stoll(frame.substr(start, end - start));
}

std::string typeOf(const std::string &frame)
{
    constexpr std::string_view marker = "\"type\":\"";
    const auto position = frame.find(marker);
    if (position == std::string::npos) return {};
    const auto start = position + marker.size();
    const auto end = frame.find('"', start);
    return frame.substr(start, end - start);
}

std::string eventIdOf(const std::string &frame)
{
    constexpr std::string_view marker = "\"eventId\":\"";
    const auto position = frame.find(marker);
    if (position == std::string::npos) return {};
    const auto start = position + marker.size();
    const auto end = frame.find('"', start);
    return frame.substr(start, end - start);
}

PeerInfo makeInfo(const std::int64_t sessionId, const std::string principal,
    const TokenKind kind)
{
    PeerInfo info;
    info.sessionId = sessionId;
    info.principalId = principal;
    info.tokenKind = kind;
    info.token = "token-" + std::to_string(sessionId);
    return info;
}

class TestRunner final {
public:
    void check(const bool condition, const std::string_view message)
    {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            ++failures_;
        }
    }

    int result() const { return failures_ == 0 ? 0 : 1; }

private:
    int failures_ = 0;
};

} // namespace

int main()
{
    TestRunner tests;

    // Scope isolation: user A, user B, admin, dashboard, exact-session.
    {
        EventHub hub;
        const auto userA = std::make_shared<FakePeer>(1);
        const auto userB = std::make_shared<FakePeer>(2);
        const auto admin = std::make_shared<FakePeer>(3);
        const auto dashboard = std::make_shared<FakePeer>(4);
        tests.check(
            hub.registerPeer(userA, makeInfo(1, "user:101", TokenKind::User))
                && hub.registerPeer(userB, makeInfo(2, "user:102", TokenKind::User))
                && hub.registerPeer(admin, makeInfo(3, "admin:1", TokenKind::Administrator))
                && hub.registerPeer(dashboard, makeInfo(4, "dashboard:1", TokenKind::Dashboard)),
            "four distinct peers register");

        hub.publish("flow.updated", "{}", EventScope{101, std::nullopt, true, false}, 1788235200);
        tests.check(userA->sent() == 1 && admin->sent() == 1,
            "user-and-admin scope reaches the target user and the administrator");
        tests.check(userB->sent() == 0 && dashboard->sent() == 0,
            "user-and-admin scope skips other users and dashboards");

        hub.publish("session.ready", "{}", EventScope{std::nullopt, 2, false, false}, 1788235200);
        tests.check(userB->sent() == 1 && userA->sent() == 1,
            "exact-session scope reaches only that session");

        hub.publish("dashboard.refresh", "{}", EventScope{std::nullopt, std::nullopt, false, true},
            1788235200);
        tests.check(dashboard->sent() == 1 && userA->sent() == 1 && admin->sent() == 1,
            "dashboard scope reaches only dashboards");
    }

    // Sequence monotonicity across event types; session.ready consumes one.
    {
        EventHub hub;
        const auto peer = std::make_shared<FakePeer>(10);
        hub.registerPeer(peer, makeInfo(10, "user:201", TokenKind::User));
        hub.publish("session.ready", "{}", EventScope{std::nullopt, 10, false, false}, 1788235200);
        hub.publish("flow.updated", "{}", EventScope{201, std::nullopt, false, false}, 1788235200);
        hub.publish("charge.progress", "{}", EventScope{201, std::nullopt, false, false},
            1788235200);
        tests.check(peer->sent() == 3, "three frames reach the single peer");
        tests.check(
            peer->events().size() == 3
                && sequenceOf(peer->events()[0]) == 1
                && sequenceOf(peer->events()[1]) == 2
                && sequenceOf(peer->events()[2]) == 3,
            "sequences are strictly monotonic across event types");
        tests.check(hub.currentSequence() == 3, "hub tracks the global sequence");
    }

    // eventId: format, same-day serial increment, UTC day rollover.
    {
        EventHubOptions idOptions;
        idOptions.clock = [] { return std::int64_t{1788235200}; };
        EventHub hub(idOptions);
        const auto peer = std::make_shared<FakePeer>(20);
        hub.registerPeer(peer, makeInfo(20, "user:301", TokenKind::User));
        // 1788235200 = 2026-09-01T04:00:00Z
        hub.publish("flow.updated", "{}", EventScope{301, std::nullopt, false, false}, 1788235200);
        hub.publish("flow.updated", "{}", EventScope{301, std::nullopt, false, false},
            1788148800);  // delayed outbox event from the previous day
        const auto firstId = eventIdOf(peer->events()[0]);
        const auto secondId = eventIdOf(peer->events()[1]);
        tests.check(firstId == "EV2026090100000001", "first eventId carries the UTC date and serial");
        tests.check(secondId == "EV2026090100000002",
            "a delayed old event cannot move the allocation day backwards");

        std::int64_t allocationTime = 1788220799;
        EventHubOptions rolloverOptions;
        rolloverOptions.clock = [&allocationTime] { return allocationTime; };
        EventHub rolloverHub(rolloverOptions);
        const auto rolloverPeer = std::make_shared<FakePeer>(21);
        rolloverHub.registerPeer(rolloverPeer, makeInfo(21, "user:302", TokenKind::User));
        rolloverHub.publish("flow.updated", "{}", EventScope{302, std::nullopt, false, false},
            1788220799);  // 2026-08-31T23:59:59Z
        allocationTime = 1788220800;
        rolloverHub.publish("flow.updated", "{}", EventScope{302, std::nullopt, false, false},
            1788220800);  // 2026-09-01T00:00:00Z
        tests.check(
            eventIdOf(rolloverPeer->events()[0]).rfind("EV20260831", 0) == 0,
            "eventId carries the previous UTC day before midnight");
        tests.check(
            eventIdOf(rolloverPeer->events()[1]) == "EV2026090100000001",
            "UTC day rollover changes the date and resets the serial");
    }

    // eventId stays unique beyond 10000 events in one UTC day (13.2).
    {
        EventHubOptions options;
        options.maxWindowFrames = 20000;
        options.clock = [] { return std::int64_t{1788235200}; };
        EventHub hub(options);
        const auto peer = std::make_shared<FakePeer>(22);
        hub.registerPeer(peer, makeInfo(22, "user:303", TokenKind::User));
        const std::int64_t dayStart = 1788235200;  // 2026-09-01T04:00:00Z
        for (int i = 0; i < 10002; ++i) {
            hub.publish("flow.updated", "{}",
                EventScope{303, std::nullopt, false, false}, dayStart + i % 3600);
        }
        std::set<std::string> seen;
        bool allUnique = true;
        for (const auto &event : peer->events()) {
            if (!seen.insert(eventIdOf(event)).second) {
                allUnique = false;
                break;
            }
        }
        tests.check(
            peer->events().size() == 10002 && allUnique
                && eventIdOf(peer->events()[9999]) == "EV2026090100010000",
            "eventIds stay unique past the former four-digit serial limit");
    }

    // Backpressure window: non-progress overflow closes with 1013; progress
    // frames are dropped instead; progress frames free capacity first.
    {
        EventHub hub;
        const auto peer = std::make_shared<FakePeer>(30);
        hub.registerPeer(peer, makeInfo(30, "user:401", TokenKind::User));
        for (int i = 0; i < 256; ++i) {
            hub.publish("flow.updated", "{}", EventScope{401, std::nullopt, false, false},
                1788235200);
        }
        tests.check(peer->sent() == 256 && peer->closed() == 0,
            "the first 256 frames fill the window without closing");
        hub.publish("flow.updated", "{}", EventScope{401, std::nullopt, false, false},
            1788235200);
        tests.check(
            peer->sent() == 256 && peer->closed() == 1
                && peer->closes().front().first == 1013,
            "the 257th non-progress frame closes the peer with 1013");

        EventHub progressHub;
        const auto progressPeer = std::make_shared<FakePeer>(31);
        progressHub.registerPeer(progressPeer, makeInfo(31, "user:402", TokenKind::User));
        for (int i = 0; i < 256; ++i) {
            progressHub.publish("flow.updated", "{}", EventScope{402, std::nullopt, false, false},
                1788235200);
        }
        for (int i = 0; i < 10; ++i) {
            progressHub.publish("charge.progress", "{}",
                EventScope{402, std::nullopt, false, false}, 1788235200);
        }
        tests.check(
            progressPeer->sent() == 256 && progressPeer->closed() == 0,
            "overflowing progress frames are dropped while the peer stays connected");

        EventHub freeingHub;
        const auto freeingPeer = std::make_shared<FakePeer>(32);
        freeingHub.registerPeer(freeingPeer, makeInfo(32, "user:403", TokenKind::User));
        for (int i = 0; i < 256; ++i) {
            freeingHub.publish("charge.progress", "{}",
                EventScope{403, std::nullopt, false, false}, 1788235200);
        }
        freeingHub.publish("flow.updated", "{}", EventScope{403, std::nullopt, false, false},
            1788235200);
        tests.check(
            freeingPeer->sent() == 257 && freeingPeer->closed() == 0,
            "a non-progress frame frees capacity by discarding old progress frames");
    }

    // Heartbeat: ping cadence, pong reset, pong timeout, liveness re-auth.
    {
        EventHubOptions options;
        options.livenessCheck = [](const std::string_view token) {
            return token != "token-51";
        };
        EventHub hub(options);
        const auto alive = std::make_shared<FakePeer>(50);
        const auto stale = std::make_shared<FakePeer>(51);
        const auto idle = std::make_shared<FakePeer>(52);
        hub.registerPeer(alive, makeInfo(50, "user:501", TokenKind::User));
        hub.registerPeer(stale, makeInfo(51, "user:502", TokenKind::User));
        hub.registerPeer(idle, makeInfo(52, "user:503", TokenKind::User));

        hub.tickHeartbeat(1000);
        tests.check(alive->sent() == 0, "no ping before the silence interval");

        hub.tickHeartbeat(1030);
        tests.check(
            alive->sent() == 1 && alive->events().front() == "{\"type\":\"ping\"}",
            "a ping frame is sent after thirty seconds of silence");
        tests.check(stale->sent() == 1 && idle->sent() == 1, "all silent peers get pings");
        tests.check(
            alive->closed() == 0 && stale->closed() == 1
                && stale->closes().front().first == 4002
                && idle->closed() == 0,
            "liveness re-auth closes stale sessions with 4002 and keeps valid ones");

        hub.recordPong(50, 1040, alive.get());
        hub.recordPong(52, 1040, idle.get());
        hub.tickHeartbeat(1071);
        tests.check(alive->sent() == 2, "a pong resets the silence and allows the next ping");
        tests.check(idle->sent() == 2 && idle->closed() == 0,
            "a ponging peer is not treated as idle");

        hub.recordPong(50, 1100, alive.get());
        hub.tickHeartbeat(1101);
        tests.check(
            idle->closed() == 1 && idle->closes().front().first == 4000,
            "no pong within the timeout closes with 4000");
        tests.check(alive->closed() == 0, "the ponging peer survives");
    }

    // Revocation: frame before close 4001; other peers unaffected.
    {
        EventHub hub;
        const auto target = std::make_shared<FakePeer>(60);
        const auto bystander = std::make_shared<FakePeer>(61);
        hub.registerPeer(target, makeInfo(60, "user:601", TokenKind::User));
        hub.registerPeer(bystander, makeInfo(61, "user:602", TokenKind::User));
        hub.notifySessionRevoked(60, "user:601");
        tests.check(
            target->events().size() == 1
                && typeOf(target->events().front()) == "session.revoked"
                && target->closes().size() == 1
                && target->closes().front().first == 4001,
            "the revoked peer receives the frame and then close 4001");
        tests.check(
            bystander->sent() == 0 && bystander->closed() == 0,
            "other peers are not affected by the revocation");
        tests.check(hub.peerCount() == 1, "the revoked peer leaves the registry");
    }

    // Registry rules: same-session replacement, capacity, identity-based
    // unregister, shutdown.
    {
        EventHubOptions options;
        options.maxPeers = 2;
        EventHub hub(options);
        const auto first = std::make_shared<FakePeer>(70);
        const auto second = std::make_shared<FakePeer>(71);
        const auto third = std::make_shared<FakePeer>(72);
        tests.check(hub.registerPeer(first, makeInfo(70, "user:701", TokenKind::User)),
            "first peer registers");
        tests.check(hub.registerPeer(second, makeInfo(70, "user:702", TokenKind::User)),
            "a duplicate session id replaces the old peer instead of failing");
        tests.check(
            first->closed() == 1 && first->closes().front().first == 1001,
            "the replaced peer is closed with 1001");
        tests.check(hub.peerCount() == 1, "replacement keeps the registry size");
        tests.check(
            hub.publish("flow.updated", "{}",
                EventScope{702, std::nullopt, false, false}, 1788235200)
                && second->sent() == 1 && first->sent() == 0,
            "events reach the replacement, not the replaced peer");
        // The replaced connection's onclose fires after the takeover; it must
        // not remove the replacement.
        hub.unregisterPeer(70, first.get());
        tests.check(hub.peerCount() == 1,
            "a replaced peer's unregister cannot remove the replacement");
        hub.unregisterPeer(70, second.get());
        tests.check(hub.peerCount() == 0,
            "an identity-matched unregister removes the entry");
        tests.check(hub.registerPeer(second, makeInfo(71, "user:702", TokenKind::User)),
            "a distinct session registers");
        const auto fourth = std::make_shared<FakePeer>(73);
        tests.check(hub.registerPeer(fourth, makeInfo(72, "user:703", TokenKind::User)),
            "the second slot registers");
        tests.check(
            !hub.registerPeer(third, makeInfo(73, "user:704", TokenKind::User)),
            "registration beyond the peer capacity is rejected");
        tests.check(hub.canAcceptPeer(72) && !hub.canAcceptPeer(73),
            "capacity preflight permits only a same-session takeover when full");
        // A same-session takeover still works at full capacity.
        const auto replacement = std::make_shared<FakePeer>(74);
        tests.check(
            hub.registerPeer(replacement, makeInfo(72, "user:705", TokenKind::User)),
            "a takeover replaces an existing session even at capacity");
        tests.check(
            fourth->closed() == 1 && fourth->closes().front().first == 1001,
            "the taken-over peer is closed with 1001");
        hub.unregisterPeer(71, second.get());
        hub.unregisterPeer(71, second.get());
        tests.check(hub.peerCount() == 1, "unregister is idempotent");

        hub.shutdown();
        tests.check(
            !hub.publish("flow.updated", "{}", EventScope{702, std::nullopt, false, false},
                1788235200)
                && second->sent() == 1,
            "a shut-down hub rejects publishes without delivering");
        tests.check(
            !hub.registerPeer(third, makeInfo(73, "user:704", TokenKind::User)),
            "a shut-down hub rejects registrations");
    }

    // closeSession: closes without a frame and removes the peer (the
    // revocation-notification fallback path).
    {
        EventHub hub;
        const auto peer = std::make_shared<FakePeer>(95);
        const auto bystander = std::make_shared<FakePeer>(96);
        hub.registerPeer(peer, makeInfo(95, "user:951", TokenKind::User));
        hub.registerPeer(bystander, makeInfo(96, "user:952", TokenKind::User));
        hub.closeSession(95, 4001, "session revoked");
        hub.closeSession(95, 4001, "session revoked");  // idempotent
        tests.check(
            peer->closes().size() == 1
                && peer->closes().front().first == 4001
                && peer->events().empty(),
            "closeSession closes the exact session once without a frame");
        tests.check(
            hub.peerCount() == 1 && bystander->closed() == 0,
            "other peers are unaffected by closeSession");
    }

    // A replaced connection's pong must not refresh the replacement.
    {
        EventHub hub;
        const auto oldPeer = std::make_shared<FakePeer>(90);
        const auto newPeer = std::make_shared<FakePeer>(91);
        hub.registerPeer(oldPeer, makeInfo(90, "user:901", TokenKind::User));
        hub.registerPeer(newPeer, makeInfo(90, "user:902", TokenKind::User));
        hub.tickHeartbeat(1000);
        hub.tickHeartbeat(1030);
        tests.check(newPeer->sent() == 1,
            "the replacement is pinged after thirty seconds of silence");
        hub.recordPong(90, 1040, oldPeer.get());
        hub.tickHeartbeat(1071);
        tests.check(
            newPeer->closed() == 1 && newPeer->closes().front().first == 4000,
            "a replaced connection's pong does not refresh the replacement");
    }
    {
        EventHub hub;
        const auto oldPeer = std::make_shared<FakePeer>(92);
        const auto newPeer = std::make_shared<FakePeer>(93);
        hub.registerPeer(oldPeer, makeInfo(92, "user:903", TokenKind::User));
        hub.registerPeer(newPeer, makeInfo(92, "user:904", TokenKind::User));
        hub.tickHeartbeat(1000);
        hub.tickHeartbeat(1030);
        hub.recordPong(92, 1040, newPeer.get());
        hub.tickHeartbeat(1071);
        tests.check(newPeer->sent() == 2 && newPeer->closed() == 0,
            "the replacement's own pong refreshes it");
    }

    // JSON escaping of event types.
    {
        EventHub hub;
        const auto peer = std::make_shared<FakePeer>(80);
        hub.registerPeer(peer, makeInfo(80, "user:801", TokenKind::User));
        hub.publish("flow.updated\"evil", "{}", EventScope{801, std::nullopt, false, false},
            1788235200);
        tests.check(
            peer->events().size() == 1
                && peer->events().front().find("\\\"evil") != std::string::npos,
            "event types are JSON-escaped in the envelope");
    }

    return tests.result();
}
