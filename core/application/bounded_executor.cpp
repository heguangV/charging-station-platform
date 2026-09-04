#include "core/application/bounded_executor.h"

#include <stdexcept>
#include <utility>

namespace ncs::core::application {

BoundedExecutor::BoundedExecutor(const std::size_t workerCount,
                                 const std::size_t queueCapacity)
    : queueCapacity_(queueCapacity) {
  if (workerCount == 0 || queueCapacity == 0) {
    throw std::invalid_argument(
        "bounded executor requires workers and queue capacity");
  }
  workers_.reserve(workerCount);
  for (std::size_t index = 0; index < workerCount; ++index) {
    workers_.emplace_back([this] { runWorker(); });
  }
}

BoundedExecutor::~BoundedExecutor() { shutdown(); }

void BoundedExecutor::shutdown() {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      // Another caller may already have requested shutdown. Joining
      // below is still required by the owning thread.
    }
    stopping_ = true;
  }
  ready_.notify_all();
  for (auto &worker : workers_) {
    if (worker.joinable())
      worker.join();
  }
}

bool BoundedExecutor::submit(std::function<void()> task) {
  if (!task)
    return false;
  {
    std::lock_guard lock(mutex_);
    if (stopping_ || tasks_.size() >= queueCapacity_)
      return false;
    tasks_.push_back(std::move(task));
  }
  ready_.notify_one();
  return true;
}

std::size_t BoundedExecutor::pending() const {
  std::lock_guard lock(mutex_);
  return tasks_.size();
}

void BoundedExecutor::runWorker() {
  while (true) {
    std::function<void()> task;
    {
      std::unique_lock lock(mutex_);
      ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
      if (stopping_ && tasks_.empty())
        return;
      task = std::move(tasks_.front());
      tasks_.pop_front();
    }
    try {
      task();
    } catch (...) {
      // A task failure must not terminate a long-lived worker.
    }
  }
}

} // namespace ncs::core::application
