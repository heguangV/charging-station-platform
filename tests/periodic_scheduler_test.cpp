#include "server/runtime/periodic_scheduler.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

using ncs::server::runtime::PeriodicScheduler;
using Clock = std::chrono::steady_clock;

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
    const auto t0 = Clock::now();

    // Cadence: due/not-due and first-tick behavior.
    {
        PeriodicScheduler scheduler;
        int fastRuns = 0;
        int slowRuns = 0;
        scheduler.add(std::chrono::seconds(2), [&fastRuns](auto done) {
            ++fastRuns;
            done();
        });
        scheduler.add(std::chrono::seconds(10), [&slowRuns](auto done) {
            ++slowRuns;
            done();
        });
        scheduler.tick(t0);
        tests.check(fastRuns == 1 && slowRuns == 1,
            "all entries fire on the first tick");
        scheduler.tick(t0 + std::chrono::seconds(1));
        tests.check(fastRuns == 1 && slowRuns == 1,
            "nothing fires before its interval elapses");
        scheduler.tick(t0 + std::chrono::seconds(2));
        tests.check(fastRuns == 2 && slowRuns == 1,
            "only the due entry fires at its cadence");
    }

    // In-flight guard: a deferred done() blocks re-entry.
    {
        PeriodicScheduler scheduler;
        int runs = 0;
        std::function<void()> pendingDone;
        scheduler.add(std::chrono::seconds(1), [&runs, &pendingDone](auto done) {
            ++runs;
            pendingDone = done;
        });
        scheduler.tick(t0);
        scheduler.tick(t0 + std::chrono::seconds(5));
        scheduler.tick(t0 + std::chrono::seconds(10));
        tests.check(runs == 1, "an in-flight entry is not re-entered");
        pendingDone();
        scheduler.tick(t0 + std::chrono::seconds(11));
        tests.check(runs == 2, "done() releases the entry for the next run");
    }

    // Interval counting resumes from the completion tick, not the start tick.
    {
        PeriodicScheduler scheduler;
        int runs = 0;
        std::function<void()> pendingDone;
        scheduler.add(std::chrono::seconds(2), [&runs, &pendingDone](auto done) {
            ++runs;
            pendingDone = done;
        });
        scheduler.tick(t0);
        pendingDone();
        scheduler.tick(t0 + std::chrono::seconds(1));
        scheduler.tick(t0 + std::chrono::seconds(3));
        tests.check(runs == 2, "the interval is measured from the completion tick");
    }

    // A throwing entry is released instead of stalling forever.
    {
        PeriodicScheduler scheduler;
        int runs = 0;
        scheduler.add(std::chrono::seconds(2), [&runs](auto) {
            ++runs;
            throw std::runtime_error("transient failure");
        });
        scheduler.tick(t0);
        tests.check(runs == 1, "the throwing entry runs once");
        scheduler.tick(t0 + std::chrono::seconds(1));
        tests.check(runs == 1, "it does not re-run before its interval");
        scheduler.tick(t0 + std::chrono::seconds(2));
        tests.check(runs == 2, "the entry runs again after a throw instead of stalling");
    }

    return tests.result();
}
