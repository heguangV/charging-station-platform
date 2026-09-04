#include "core/application/charging_repository.h"
#include "core/application/event_hub.h"
#include "server/websocket/outbox_dispatcher.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace ncs::core::application;
using ncs::server::websocket::OutboxDispatcher;

class FakePeer final : public WebSocketPeer {
public:
    void sendText(std::string frame) override { frames_.push_back(std::move(frame)); }
    void close(const std::uint16_t code, std::string reason) override
    {
        closes_.emplace_back(code, std::move(reason));
    }
    const std::vector<std::string> &frames() const { return frames_; }
    const std::vector<std::pair<std::uint16_t, std::string>> &closes() const
    {
        return closes_;
    }

private:
    std::vector<std::string> frames_;
    std::vector<std::pair<std::uint16_t, std::string>> closes_;
};

std::string typeOf(const std::string &frame)
{
    constexpr std::string_view marker = "\"type\":\"";
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
    const auto now = std::chrono::system_clock::time_point(std::chrono::seconds(1788500000));
    constexpr std::int64_t t0 = 1788500000;

    InMemoryChargingRepository repository;
    auto hub = std::make_shared<EventHub>();
    OutboxDispatcher dispatcher(repository, hub);

    const auto userA = std::make_shared<FakePeer>();
    const auto userB = std::make_shared<FakePeer>();
    const auto admin = std::make_shared<FakePeer>();
    const auto dashboard = std::make_shared<FakePeer>();
    tests.check(
        hub->registerPeer(userA, makeInfo(1, "user:1001", TokenKind::User))
            && hub->registerPeer(userB, makeInfo(2, "user:1002", TokenKind::User))
            && hub->registerPeer(admin, makeInfo(3, "admin:1", TokenKind::Administrator))
            && hub->registerPeer(dashboard, makeInfo(4, "dashboard:1", TokenKind::Dashboard)),
        "four peers register for dispatcher routing");

    ChargingFlow flow;
    flow.flowNo = "FLTEST000001";
    flow.userId = 1001;
    flow.stationId = 1;
    flow.chargerType = ChargerType::DcFast;
    flow.status = static_cast<int>(FlowStatus::Queued);
    flow.createdAt = t0;
    repository.addFlow(flow);

    ChargingOrder order;
    order.orderNo = "ORTEST000001";
    order.flowNo = "FLTEST000001";
    order.userId = 1001;
    order.stationId = 1;
    order.stationName = "NCS 中关村充电站";
    order.chargerId = 1;
    order.chargerCode = "ZGC-DC-01";
    order.chargerType = ChargerType::DcFast;
    order.energyMwh = 12345678;
    order.amountCent = 4321;
    order.paidCent = 4000;
    order.debtAddedCent = 321;
    order.balanceAfterCent = 0;
    order.debtAfterCent = 321;
    order.settledAt = t0 + 60;
    order.status = static_cast<int>(FlowStatus::Completed);
    repository.addOrder(order);

    // flow.updated: reaches the target user and admins, not the other user.
    repository.addFlowEvent(FlowEvent{"FLTEST000001", 10, 20, "CREATED", t0 + 1});
    tests.check(dispatcher.dispatchOnce(now + std::chrono::seconds(2)) == 1,
        "one flow event is dispatched");
    tests.check(
        userA->frames().size() == 1 && typeOf(userA->frames().front()) == "flow.updated"
            && userA->frames().front().find("\"flowNo\":\"FLTEST000001\"")
                != std::string::npos
            && userA->frames().front().find("\"toStatus\":20") != std::string::npos
            && userA->frames().front().find("\"statusText\":\"待报价确认\"")
                != std::string::npos
            && userA->frames().front().find("\"stationName\":\"NCS 中关村充电站\"")
                != std::string::npos,
        "the target user receives the enriched flow.updated frame");
    tests.check(admin->frames().size() == 1, "the administrator receives flow.updated");
    tests.check(userB->frames().empty(), "the other user receives nothing");
    tests.check(dashboard->frames().size() == 1 && typeOf(dashboard->frames().front())
            == "dashboard.refresh",
        "a delivered business event publishes dashboard.refresh");

    // order.settled: target user + admins; no receipt fields leak.
    repository.addFlowEvent(FlowEvent{"FLTEST000001", 50, 60, "USER_STOPPED", t0 + 60});
    tests.check(dispatcher.dispatchOnce(now + std::chrono::seconds(61)) == 1,
        "one settlement event is dispatched");
    const auto &userFrames = userA->frames();
    const auto &settledFrame = userFrames.back();
    tests.check(
        typeOf(settledFrame) == "order.settled"
            && settledFrame.find("\"orderNo\":\"ORTEST000001\"") != std::string::npos
            && settledFrame.find("\"amountCent\":4321") != std::string::npos,
        "order.settled carries the settlement essentials");
    tests.check(
        settledFrame.find("paidCent") == std::string::npos
            && settledFrame.find("debtAddedCent") == std::string::npos
            && settledFrame.find("balanceAfterCent") == std::string::npos
            && settledFrame.find("stationName") == std::string::npos,
        "order.settled omits receipt fields and station names");
    tests.check(
        admin->frames().back().find("order.settled") != std::string::npos,
        "the administrator also receives order.settled");
    tests.check(userB->frames().empty(), "the other user still receives nothing");

    // charger.statusChanged: admins only.
    repository.addChargerStatusEvent(
        ChargerStatusEvent{1, 1, 0, 1, "ALLOCATED", t0 + 61});
    tests.check(dispatcher.dispatchOnce(now + std::chrono::seconds(62)) == 1,
        "one charger event is dispatched");
    tests.check(
        typeOf(admin->frames().back()) == "charger.statusChanged"
            && admin->frames().back().find("\"chargerId\":1") != std::string::npos
            && admin->frames().back().find("\"chargerCode\":\"ZGC-DC-01\"")
                != std::string::npos
            && admin->frames().back().find("\"reason\":\"ALLOCATED\"")
                != std::string::npos,
        "the administrator receives charger.statusChanged with the charger fields");
    tests.check(
        userA->frames().size() == 2 && userB->frames().empty(),
        "users do not receive charger status events");

    // dashboard.refresh throttling: batches closer than five seconds to the
    // last refresh (t0+61) do not publish another refresh.
    const auto refreshFramesBefore = dashboard->frames().size();
    repository.addChargerStatusEvent(
        ChargerStatusEvent{2, 1, 0, 1, "ALLOCATED", t0 + 62});
    dispatcher.dispatchOnce(now + std::chrono::seconds(63));
    tests.check(
        dashboard->frames().size() == refreshFramesBefore,
        "a batch within the throttle window publishes no refresh");
    repository.addChargerStatusEvent(
        ChargerStatusEvent{3, 1, 0, 1, "ALLOCATED", t0 + 63});
    dispatcher.dispatchOnce(now + std::chrono::seconds(70));
    tests.check(
        dashboard->frames().size() == refreshFramesBefore + 1,
        "a batch after the throttle window publishes another refresh");

    // A shut-down hub leaves rows pending for redelivery after restart.
    const auto pendingBefore = repository.pollOutbox(t0 + 100, 100);
    repository.addChargerStatusEvent(
        ChargerStatusEvent{4, 1, 0, 1, "ALLOCATED", t0 + 64});
    hub->shutdown();
    tests.check(dispatcher.dispatchOnce(now + std::chrono::seconds(71)) == 0,
        "a shut-down hub delivers nothing");
    tests.check(
        repository.pollOutbox(t0 + 100, 100).size() == pendingBefore.size() + 1,
        "undelivered rows stay pending for the next process run");

    return tests.result();
}
