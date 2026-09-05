#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <vector>

namespace ncs::server::runtime {

// Multi-cadence periodic scheduler driven by one crow tick (crow only keeps
// a single tick slot). tick() must only be called from the tick callback on
// the acceptor io thread; done() may be called from worker threads, so the
// in-flight flag is atomic.
class PeriodicScheduler final {
public:
    // work receives a done() callback; an entry may not run again until the
    // previous run called done(). Inline work calls done() synchronously;
    // work handed to a worker pool passes done() into the submitted lambda
    // (or calls it immediately when the submission is rejected). Work that
    // throws before calling done() is caught by the scheduler so a transient
    // failure cannot stall the entry forever.
    using Work = std::function<void(std::function<void()> done)>;

    void add(std::chrono::seconds interval, Work work);
    void tick(std::chrono::steady_clock::time_point now);

private:
    struct Entry {
        std::chrono::seconds interval;
        Work work;
        std::chrono::steady_clock::time_point lastRun{};
        std::atomic<bool> inFlight{false};
    };

    // Stable addresses: done() callbacks capture the entry pointer, which
    // must survive vector growth.
    std::vector<std::unique_ptr<Entry>> entries_;
};

} // namespace ncs::server::runtime
