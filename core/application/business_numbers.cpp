#include "core/application/business_numbers.h"

#include <cstdio>
#include <ctime>

namespace ncs::core::application {

std::string
BusinessNumbers::next(const std::string_view prefix,
                      const std::chrono::system_clock::time_point now) {
  std::lock_guard lock(mutex_);
  const std::int64_t day = utcDayIndex(now);
  if (day != lastDay_) {
    lastDay_ = day;
    counters_.clear();
  }
  const std::int64_t sequence = store_
                                    ? store_->nextBusinessSequence(prefix, day)
                                    : ++counters_[std::string(prefix)];
  const std::time_t utc = std::chrono::system_clock::to_time_t(now);
  std::tm dayParts{};
  gmtime_r(&utc, &dayParts);
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%04d%02d%02d%04lld",
                dayParts.tm_year + 1900, dayParts.tm_mon + 1, dayParts.tm_mday,
                static_cast<long long>(sequence));
  return std::string(prefix) + buffer;
}

std::int64_t
BusinessNumbers::utcDayIndex(const std::chrono::system_clock::time_point now) {
  return std::chrono::duration_cast<std::chrono::hours>(now.time_since_epoch())
             .count() /
         24;
}

} // namespace ncs::core::application
