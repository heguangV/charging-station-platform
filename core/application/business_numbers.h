#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ncs::core::application {

class BusinessNumberSequenceStore {
public:
  virtual ~BusinessNumberSequenceStore() = default;
  virtual std::int64_t nextBusinessSequence(std::string_view prefix,
                                            std::int64_t utcDay) = 0;
};

// Generates business numbers like "RC202609030001": prefix + UTC date +
// per-prefix daily sequence. Not a UUID; uniqueness relies on the per-prefix
// counter plus the unique constraint of the future persistent store.
class BusinessNumbers final {
public:
  explicit BusinessNumbers(BusinessNumberSequenceStore *store = nullptr)
      : store_(store) {}

  std::string next(std::string_view prefix,
                   std::chrono::system_clock::time_point now);

private:
  static std::int64_t utcDayIndex(std::chrono::system_clock::time_point now);

  std::mutex mutex_;
  std::int64_t lastDay_ = -1;
  std::unordered_map<std::string, int> counters_;
  BusinessNumberSequenceStore *store_ = nullptr;
};

} // namespace ncs::core::application
