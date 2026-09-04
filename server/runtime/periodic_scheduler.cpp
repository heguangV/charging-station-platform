#include "server/runtime/periodic_scheduler.h"

#include <utility>

namespace ncs::server::runtime {

void PeriodicScheduler::add(std::chrono::seconds interval, Work work)
{
    auto entry = std::make_unique<Entry>();
    entry->interval = interval;
    entry->work = std::move(work);
    entries_.push_back(std::move(entry));
}

void PeriodicScheduler::tick(const std::chrono::steady_clock::time_point now)
{
    for (auto &entry : entries_) {
        if (entry->inFlight.load(std::memory_order_acquire)
            || now - entry->lastRun < entry->interval) continue;
        entry->inFlight.store(true, std::memory_order_release);
        entry->lastRun = now;
        try {
            entry->work([entry = entry.get()] {
                entry->inFlight.store(false, std::memory_order_release);
            });
        } catch (...) {
            // Inline work that throws before calling done() must not leave
            // the entry stalled; it simply runs again at the next interval.
            entry->inFlight.store(false, std::memory_order_release);
        }
    }
}

} // namespace ncs::server::runtime
