// 有界线程池：固定 worker 数量 + 有界 FIFO 任务队列，队列满时 submit 直接返回 false 形成背压。
// 用于隔离服务端异步任务（如会话吊销通知），防止线程与队列无界增长、不阻塞事件循环。
// 约束：不可复制；shutdown/析构后 worker 排空剩余任务再退出，任务异常被吞掉以保证线程存活。

#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace ncs::core::application
{

class BoundedExecutor final
{
  public:
    // 启动固定数量 worker；workerCount 或 queueCapacity 为 0 时抛 std::invalid_argument。
    BoundedExecutor(std::size_t workerCount, std::size_t queueCapacity);
    ~BoundedExecutor();

    BoundedExecutor(const BoundedExecutor&) = delete;
    BoundedExecutor& operator=(const BoundedExecutor&) = delete;

    // 提交任务：队列已满或已 shutdown 时立即返回 false（背压，不阻塞调用线程），空任务同样拒绝。
    bool submit(std::function<void()> task);
    std::size_t pending() const;
    // 停止接收新任务并等待 worker 排空剩余任务后退出（幂等可重复调用；析构函数自动调用）。
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
