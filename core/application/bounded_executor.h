#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace ncs::core::application {

class BoundedExecutor final {
public:
  BoundedExecutor(std::size_t workerCount, std::size_t queueCapacity);
  ~BoundedExecutor();

  BoundedExecutor(const BoundedExecutor &) = delete;
  BoundedExecutor &operator=(const BoundedExecutor &) = delete;

  bool submit(std::function<void()> task);
  std::size_t pending() const;
  void shutdown();

private:
  void runWorker();

  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<std::function<void()>> tasks_;
  std::vector<std::thread> workers_;
  std::size_t queueCapacity_;
  bool stopping_ = false;
};

} // namespace ncs::core::application
