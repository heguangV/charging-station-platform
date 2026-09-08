// 业务编号生成器：按“前缀 + UTC 日期 + 每前缀当日序号”生成形如 RC202609030001
// 的业务编号（FL/OR/RC/WT 等），非 UUID。 内存计数器加互斥锁并处理 UTC 日切换；可注入
// BusinessNumberSequenceStore 做序号持久化（生产为 SQLite 序列存储）。
// 约束：唯一性依赖每前缀计数器加持久层唯一约束兜底；用于充电流程、订单、充值单等业务编号。

#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ncs::core::application
{

class BusinessNumberSequenceStore
{
  public:
    virtual ~BusinessNumberSequenceStore() = default;
    // 端口方法：返回指定前缀在 utcDay（UTC
    // 天序号）当日的下一个序号；实现须保证并发安全且跨进程唯一。
    virtual std::int64_t nextBusinessSequence(std::string_view prefix, std::int64_t utcDay) = 0;
};

// Generates business numbers like "RC202609030001": prefix + UTC date +
// per-prefix daily sequence. Not a UUID; uniqueness relies on the per-prefix
// counter plus the unique constraint of the future persistent store.
class BusinessNumbers final
{
  public:
    explicit BusinessNumbers(BusinessNumberSequenceStore* store = nullptr) : store_(store) {}

    // 生成“前缀 + UTC 日期 + 当日序号”的业务编号（如 RC202609030001）；线程安全，UTC
    // 日切换自动清零内存计数器， 注入 store 时序号改由持久层发放（内存计数仅作兜底）。
    std::string next(std::string_view prefix, std::chrono::system_clock::time_point now);

  private:
    static std::int64_t utcDayIndex(std::chrono::system_clock::time_point now);

    std::mutex mutex_;
    std::int64_t lastDay_ = -1;
    std::unordered_map<std::string, int> counters_;
    BusinessNumberSequenceStore* store_ = nullptr;
};

} // namespace ncs::core::application
