#include "core/application/idempotency_service.h"

#include <chrono>
#include <iostream>
#include <string_view>

namespace {

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
    using namespace ncs::core::application;
    TestRunner tests;
    IdempotencyService service;
    constexpr std::string_view key = "2cb640c6-6995-4be5-9161-f0e2c1201a53";
    const auto now = std::chrono::system_clock::time_point(std::chrono::seconds(1000));

    tests.check(IdempotencyService::isUuid(key), "canonical idempotency UUID is accepted");
    tests.check(!IdempotencyService::isUuid("invalid"), "invalid idempotency key is rejected");
    tests.check(
        service.begin("user-1:recharge", "invalid", "{}", now).decision
            == IdempotencyDecision::InvalidKey,
        "begin rejects non-UUID key");
    const auto reservation = service.begin(
        "user-1:recharge", key, "{\"amountCent\":100}", now);
    tests.check(
        reservation.decision == IdempotencyDecision::Proceed
            && reservation.leaseToken.has_value(),
        "first request reserves key");
    tests.check(
        service.begin("user-1:recharge", key, "{\"amountCent\":100}", now).decision
            == IdempotencyDecision::InProgress,
        "concurrent duplicate does not execute twice");
    tests.check(
        service.begin("user-1:recharge", key, "{\"amountCent\":200}", now).decision
            == IdempotencyDecision::Conflict,
        "same key with different request conflicts");
    tests.check(
        service.complete(
            "user-1:recharge", key, *reservation.leaseToken,
            StoredHttpResult{201, "application/json; charset=utf-8", "{\"result\":1}"}, now),
        "reserved result can complete once");
    const auto replay = service.begin(
        "user-1:recharge", key, "{\"amountCent\":100}", now + std::chrono::hours(24 * 6));
    tests.check(
        replay.decision == IdempotencyDecision::Replay && replay.replay
            && replay.replay->status == 201 && replay.replay->body == "{\"result\":1}",
        "same request replays original HTTP result");
    tests.check(
        service.begin(
            "another-user:recharge", key, "{\"amountCent\":100}", now).decision
            == IdempotencyDecision::Proceed,
        "idempotency key is isolated by scope");

    constexpr std::string_view expiringKey = "2cb640c6-6995-4be5-9161-f0e2c1201a54";
    const auto expiring = service.begin("temporary", expiringKey, "{}", now);
    service.complete(
        "temporary", expiringKey, *expiring.leaseToken,
        StoredHttpResult{200, {}, "{}"}, now);
    service.cleanup(now + std::chrono::hours(24 * 8));
    tests.check(
        service.begin("temporary", expiringKey, "{}", now + std::chrono::hours(24 * 8)).decision
            == IdempotencyDecision::Proceed,
        "ordinary record expires after at least seven days");

    constexpr std::string_view permanentKey = "2cb640c6-6995-4be5-9161-f0e2c1201a55";
    const auto permanent = service.begin("settlement", permanentKey, "{}", now, true);
    service.complete(
        "settlement", permanentKey, *permanent.leaseToken,
        StoredHttpResult{200, {}, "{}"}, now);
    service.cleanup(now + std::chrono::hours(24 * 365));
    tests.check(
        service.begin("settlement", permanentKey, "{}", now + std::chrono::hours(24 * 365)).decision
            == IdempotencyDecision::Replay,
        "business-unique result is retained permanently");

    constexpr std::string_view transientKey =
        "2cb640c6-6995-4be5-9161-f0e2c1201a5b";
    const auto transient =
        service.begin("settlement", transientKey, "{}", now, true);
    tests.check(
        service.complete("settlement", transientKey, *transient.leaseToken,
                         StoredHttpResult{503, {}, "{}"}, now),
        "a transient response releases its idempotency lease");
    tests.check(
        service.begin("settlement", transientKey, "{}", now).decision ==
            IdempotencyDecision::Proceed,
        "a permanent business key can retry after a transient 5xx");

    constexpr std::string_view conflictKey =
        "2cb640c6-6995-4be5-9161-f0e2c1201a5c";
    const auto conflict =
        service.begin("settlement", conflictKey, "{}", now, true);
    tests.check(
        service.complete("settlement", conflictKey, *conflict.leaseToken,
                         StoredHttpResult{409, {}, "{}"}, now),
        "a client-error response releases its idempotency lease");
    tests.check(
        service.begin("settlement", conflictKey, "{}", now).decision ==
            IdempotencyDecision::Proceed,
        "a permanent business key can retry after a 4xx client error");

    constexpr std::string_view staleKey = "2cb640c6-6995-4be5-9161-f0e2c1201a56";
    const auto stale = service.begin("stale", staleKey, "{}", now, true);
    const auto takeover = service.begin(
        "stale", staleKey, "{}", now + std::chrono::minutes(11), true);
    tests.check(
        stale.leaseToken && takeover.decision == IdempotencyDecision::Proceed
            && takeover.leaseToken && *takeover.leaseToken != *stale.leaseToken,
        "expired in-progress reservation can be taken over even when permanent");
    tests.check(
        !service.complete(
            "stale", staleKey, *stale.leaseToken,
            StoredHttpResult{200, {}, "stale"}, now + std::chrono::minutes(11)),
        "expired reservation cannot complete a replacement lease");
    service.abort("stale", staleKey, *takeover.leaseToken);
    tests.check(
        service.begin("stale", staleKey, "{}", now + std::chrono::minutes(11)).decision
            == IdempotencyDecision::Proceed,
        "matching lease can abort an unfinished reservation");

    tests.check(checkVersion(3, 3) == VersionCheck::Match, "matching version is accepted");
    tests.check(checkVersion(2, 3) == VersionCheck::Conflict, "stale version conflicts");
    tests.check(checkVersion(0, 3) == VersionCheck::Invalid, "invalid version is rejected");

    constexpr std::string_view completedKey = "2cb640c6-6995-4be5-9161-f0e2c1201a57";
    {
        const auto reservation = service.begin("guarded", completedKey, "{}", now);
        tests.check(
            reservation.decision == IdempotencyDecision::Proceed && reservation.leaseToken,
            "guard reservation proceeds");
        IdempotencyLease guard(service, "guarded", std::string(completedKey), *reservation.leaseToken);
        tests.check(guard.valid(), "lease guard is armed");
        tests.check(
            guard.complete(StoredHttpResult{200, {}, "{}"}, now),
            "guard completes the reservation");
        tests.check(!guard.valid(), "completed guard is disarmed");
    }
    tests.check(
        service.begin("guarded", completedKey, "{}", now).decision == IdempotencyDecision::Replay,
        "guard-completed reservation replays instead of aborting");

    constexpr std::string_view exceptionKey = "2cb640c6-6995-4be5-9161-f0e2c1201a58";
    {
        const auto reservation = service.begin("guarded", exceptionKey, "{}", now);
        IdempotencyLease guard(service, "guarded", std::string(exceptionKey), *reservation.leaseToken);
        tests.check(guard.valid(), "unfinished guard is armed");
    }
    tests.check(
        service.begin("guarded", exceptionKey, "{}", now).decision == IdempotencyDecision::Proceed,
        "guard aborts the reservation when the handler leaves without completing");

    constexpr std::string_view movedKey = "2cb640c6-6995-4be5-9161-f0e2c1201a59";
    const auto movedReservation = service.begin("guarded", movedKey, "{}", now);
    IdempotencyLease owner(service, "guarded", std::string(movedKey), *movedReservation.leaseToken);
    IdempotencyLease movedTo(std::move(owner));
    tests.check(movedTo.valid() && !owner.valid(), "guard ownership moves without aborting");
    movedTo.abort();
    tests.check(!movedTo.valid(), "explicit abort disarms the guard");
    tests.check(
        service.begin("guarded", movedKey, "{}", now).decision == IdempotencyDecision::Proceed,
        "explicitly aborted reservation can be reserved again");

    constexpr std::string_view expiredKey = "2cb640c6-6995-4be5-9161-f0e2c1201a5a";
    {
        const auto reservation = service.begin("guarded", expiredKey, "{}", now);
        IdempotencyLease guard(service, "guarded", std::string(expiredKey), *reservation.leaseToken);
        service.cleanup(now + std::chrono::minutes(11));
        tests.check(
            !guard.complete(StoredHttpResult{200, {}, "{}"}, now + std::chrono::minutes(11)),
            "expired lease cannot complete");
        tests.check(guard.valid(), "failed complete keeps the guard armed");
    }
    tests.check(
        service.begin("guarded", expiredKey, "{}", now + std::chrono::minutes(11)).decision
            == IdempotencyDecision::Proceed,
        "expired guard falls back to a harmless abort");
    return tests.result();
}
