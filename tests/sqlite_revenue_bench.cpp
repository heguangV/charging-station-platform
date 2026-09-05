// NFR-P-02 microbenchmark: the 30-day revenue aggregation that feeds the
// revenue chart (DashboardService::refresh) must complete in < 300ms on the
// NFR-D-01 acceptance machine (2 cores / 4GB). The database is a real v1-v8
// migrated instance carrying the full UC-D-02 90-day demo seed (~9,000
// orders, ~3,000 inside the measured 30-day window). Everything runs in an
// isolated temporary directory and the database is removed on exit; the
// printed median/worst figures are the repeatable evidence.

#include "core/application/analytics_service.h"
#include "infrastructure/sqlite/sqlite_repository.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

std::string uniqueDatabaseName()
{
    // No getpid(): a random seed plus the steady-clock nanosecond counter
    // stays unique across processes and compiles on MSVC (the Windows CI
    // job builds and runs this benchmark).
    const auto random = std::random_device{}();
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    return "ncs-revenue-bench-" + std::to_string(random) + "-" + std::to_string(nanos) + ".db";
}

class TemporaryDatabase final
{
  public:
    TemporaryDatabase()
    {
        path_ = (std::filesystem::temp_directory_path() / uniqueDatabaseName()).string();
        cleanup();
    }

    ~TemporaryDatabase()
    {
        cleanup();
    }

    const std::string& path() const
    {
        return path_;
    }

  private:
    void cleanup() const
    {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(path_ + "-wal", ignored);
        std::filesystem::remove(path_ + "-shm", ignored);
    }

    std::string path_;
};

// Single measured refresh of the production dashboard snapshot; the revenue
// chart consumes snapshot.revenue30d produced by this exact code path.
std::pair<std::int64_t, bool> runRefresh(ncs::core::application::DashboardService& dashboard)
{
    const auto start = std::chrono::steady_clock::now();
    const auto result = dashboard.refresh(std::chrono::system_clock::now());
    const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - start)
                               .count();
    return {elapsedUs, result.ok()};
}

} // namespace

int main()
{
    constexpr int measuredRuns = 10;
    constexpr std::int64_t budgetUs = 300 * 1000; // NFR-P-02: < 300ms.

    TemporaryDatabase database;
    ncs::infrastructure::sqlite::SqliteRepository repository(database.path());
    ncs::core::application::DashboardService dashboard(repository, repository, repository,
                                                       repository);

    std::printf("NFR-P-02 revenue benchmark (30-day aggregation)\n");
    std::printf("  environment: hardware_concurrency=%u (NFR-D-01 acceptance "
                "VM: 2 cores / 4GB)\n",
                std::thread::hardware_concurrency());
    std::printf("  database: %s (isolated, removed on exit)\n", database.path().c_str());

    // Warm-up excludes first-touch page-cache and query-plan effects from the
    // measured window.
    const auto [warmupUs, warmupOk] = runRefresh(dashboard);
    std::printf("  warmup: %.2f ms (ok=%d)\n", static_cast<double>(warmupUs) / 1000.0,
                warmupOk ? 1 : 0);

    std::vector<std::int64_t> runs;
    runs.reserve(measuredRuns);
    for (int index = 0; index < measuredRuns; ++index)
    {
        const auto [elapsedUs, ok] = runRefresh(dashboard);
        if (!ok)
        {
            std::printf("  run %d failed: dashboard refresh reported an "
                        "error\n",
                        index + 1);
            return 1;
        }
        runs.push_back(elapsedUs);
    }
    std::sort(runs.begin(), runs.end());
    const std::int64_t medianUs = (runs[measuredRuns / 2 - 1] + runs[measuredRuns / 2]) / 2;
    const std::int64_t worstUs = runs.back();

    const auto snapshot = dashboard.current();
    int windowOrders = 0;
    if (snapshot)
        for (const auto& point : snapshot->revenue30d)
            windowOrders += point.orderCount;

    std::printf("  runs: %d\n", measuredRuns);
    std::printf("  median: %.2f ms  worst: %.2f ms\n", static_cast<double>(medianUs) / 1000.0,
                static_cast<double>(worstUs) / 1000.0);
    std::printf("  revenue30d buckets: %zu, orders in window: %d\n",
                snapshot ? snapshot->revenue30d.size() : 0, windowOrders);

    if (!snapshot || snapshot->revenue30d.size() != 30 || windowOrders == 0)
    {
        std::printf("FAIL: measured path did not aggregate the seeded 30-day "
                    "history\n");
        return 1;
    }
    if (worstUs >= budgetUs)
    {
        std::printf("FAIL: worst run %.2f ms >= 300 ms (NFR-P-02)\n",
                    static_cast<double>(worstUs) / 1000.0);
        return 1;
    }
    std::printf("PASS: worst run < 300 ms on the NFR-D-01 acceptance "
                "machine\n");
    return 0;
}
