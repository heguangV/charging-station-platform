// HTTP 幂等服务：对幂等键做请求摘要比对、进行中租约预约与结果重放，可接 IdempotencyPersistence
// 持久化。 供控制器包装 POST 类端点（下单、充值等）防止重复提交；内存条目上限 65536，过期条目由
// cleanup 清理。 约束：租约令牌防并发同键竞争；异常路径由 RAII 租约守卫自动
// abort，保证幂等键不被卡死。

#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ncs::core::application
{

struct StoredHttpResult
{
    int status = 200;
    std::string contentType = "application/json; charset=utf-8";
    std::string body;
};

enum class IdempotencyDecision
{
    Proceed,
    Replay,
    Conflict,
    InProgress,
    CapacityExceeded,
    InvalidKey,
};

struct IdempotencyCheck
{
    IdempotencyDecision decision = IdempotencyDecision::InvalidKey;
    std::optional<StoredHttpResult> replay;
    std::optional<std::string> leaseToken;
};

struct PersistedIdempotencyRecord
{
    std::string scope;
    std::string key;
    std::string requestDigest;
    std::optional<StoredHttpResult> result;
    std::int64_t expiresAt = 0;
    std::int64_t leaseExpiresAt = 0;
    std::string leaseToken;
    bool permanent = false;
};

class IdempotencyPersistence
{
  public:
    virtual ~IdempotencyPersistence() = default;
    // Persistent implementations override this to place the business write
    // and the completed response in one storage transaction. The default keeps
    // lightweight/in-memory adapters source-compatible.
    virtual void withTransaction(const std::function<void()>& work)
    {
        work();
    }
    virtual std::optional<PersistedIdempotencyRecord>
    loadIdempotencyRecord(std::string_view scope, std::string_view key) = 0;
    virtual void saveIdempotencyRecord(const PersistedIdempotencyRecord& record) = 0;
    virtual void removeIdempotencyRecord(std::string_view scope, std::string_view key) = 0;
    virtual void cleanupIdempotencyRecords(std::int64_t now) = 0;
    virtual std::size_t idempotencyRecordCount() = 0;
};

class IdempotencyService final
{
  public:
    explicit IdempotencyService(IdempotencyPersistence* persistence = nullptr)
        : persistence_(persistence)
    {
    }

    // ---- 生命周期：begin（查重/租约预约）→ complete（落结果供重放）/ abort ----
    // 幂等入口：scope+key 查重并比对请求体摘要（key 须为 UUID，否则 InvalidKey）。首次返回 Proceed
    // 并发放 10 分钟租约令牌； 同键同摘要已有结果返回 Replay（附带缓存响应），处理中返回
    // InProgress，摘要不同返回 Conflict，容量满返回 CapacityExceeded。
    IdempotencyCheck begin(std::string_view scope, std::string_view key,
                           std::string_view requestBody, std::chrono::system_clock::time_point now,
                           bool permanent = false);
    // 落结果供重放：仅 2xx 会存储（非 permanent 默认保留 7 天）并须匹配租约令牌；4xx/5xx
    // 释放幂等键使同键重试重新执行。
    bool complete(std::string_view scope, std::string_view key, std::string_view leaseToken,
                  StoredHttpResult result, std::chrono::system_clock::time_point now);
    // 一步完成“执行业务 +
    // 登记”：配置持久化时业务写入与结果保存共用同一存储事务（异常回滚即释放幂等键）； 4xx/5xx
    // 同样释放幂等键；租约校验失败返回 false。
    bool executeAndComplete(std::string_view scope, std::string_view key,
                            std::string_view leaseToken,
                            const std::function<StoredHttpResult()>& operation,
                            StoredHttpResult& result, std::chrono::system_clock::time_point now);
    // 释放进行中租约：删除尚未产生结果的登记项（令牌不匹配或已有结果时无副作用）；供异常路径与租约守卫析构调用。
    void abort(std::string_view scope, std::string_view key, std::string_view leaseToken);
    void cleanup(std::chrono::system_clock::time_point now);
    std::size_t size() const;

    static bool isUuid(std::string_view value);

  private:
    struct Entry
    {
        std::string requestDigest;
        std::optional<StoredHttpResult> result;
        std::chrono::system_clock::time_point expiresAt;
        std::chrono::system_clock::time_point leaseExpiresAt;
        std::string leaseToken;
        bool permanent = false;
    };

    static std::string recordKey(std::string_view scope, std::string_view key);
    void cleanupUnlocked(std::chrono::system_clock::time_point now);
    void loadFromPersistenceUnlocked(std::string_view scope, std::string_view key);
    void persistUnlocked(std::string_view scope, std::string_view key, const Entry& entry);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
    IdempotencyPersistence* persistence_ = nullptr;
    static constexpr std::size_t maximumEntries_ = 65536;
};

// RAII guard for an in-progress reservation: destroying the lease without a
// successful complete() aborts the reservation so a crashed or exception-path
// handler never leaves the key stuck for the next caller.
class IdempotencyLease final
{
  public:
    IdempotencyLease() = default;
    IdempotencyLease(IdempotencyService& service, std::string scope, std::string key,
                     std::string leaseToken);
    ~IdempotencyLease();

    IdempotencyLease(const IdempotencyLease&) = delete;
    IdempotencyLease& operator=(const IdempotencyLease&) = delete;
    IdempotencyLease(IdempotencyLease&& other) noexcept;
    IdempotencyLease& operator=(IdempotencyLease&& other) noexcept;

    bool valid() const;
    // Returns true when the stored reservation accepted the result; the guard
    // is disarmed only on success so a rejected complete() still aborts.
    bool complete(StoredHttpResult result, std::chrono::system_clock::time_point now);
    bool executeAndComplete(const std::function<StoredHttpResult()>& operation,
                            StoredHttpResult& result, std::chrono::system_clock::time_point now);
    // 显式放弃租约并解除守卫（守卫失效后析构不再触发二次 abort）。
    void abort();

  private:
    IdempotencyService* service_ = nullptr;
    std::string scope_;
    std::string key_;
    std::string leaseToken_;
};

// ---- 乐观版本比对工具：Match/Conflict/Invalid ----
enum class VersionCheck
{
    Match,
    Conflict,
    Invalid
};
// 乐观锁版本比对：任一版本 <1 返回 Invalid，相等返回 Match，否则 Conflict。
VersionCheck checkVersion(long long expectedVersion, long long currentVersion);

} // namespace ncs::core::application
