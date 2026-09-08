// 多档位周期调度器：在 Crow 唯一的 tick
// 槽位上按各自间隔驱动多个周期任务（心跳、进度推送、outbox、维护、快照、清理）。 线程约束：tick()
// 只能由 acceptor 线程的 tick 回调调用；done() 可来自工作线程，条目以原子 in-flight
// 标志防重入，异常不会永久卡死条目。
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <vector>

namespace ncs::server::runtime
{

// Multi-cadence periodic scheduler driven by one crow tick (crow only keeps
// a single tick slot). tick() must only be called from the tick callback on
// the acceptor io thread; done() may be called from worker threads, so the
// in-flight flag is atomic.
class PeriodicScheduler final
{
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
    struct Entry
    {
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
