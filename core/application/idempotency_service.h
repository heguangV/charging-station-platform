#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ncs::core::application {

struct StoredHttpResult {
  int status = 200;
  std::string contentType = "application/json; charset=utf-8";
  std::string body;
};

enum class IdempotencyDecision {
  Proceed,
  Replay,
  Conflict,
  InProgress,
  CapacityExceeded,
  InvalidKey,
};

struct IdempotencyCheck {
  IdempotencyDecision decision = IdempotencyDecision::InvalidKey;
  std::optional<StoredHttpResult> replay;
  std::optional<std::string> leaseToken;
};

struct PersistedIdempotencyRecord {
  std::string scope;
  std::string key;
  std::string requestDigest;
  std::optional<StoredHttpResult> result;
  std::int64_t expiresAt = 0;
  std::int64_t leaseExpiresAt = 0;
  std::string leaseToken;
  bool permanent = false;
};

class IdempotencyPersistence {
public:
  virtual ~IdempotencyPersistence() = default;
  // Persistent implementations override this to place the business write
  // and the completed response in one storage transaction. The default keeps
  // lightweight/in-memory adapters source-compatible.
  virtual void withTransaction(const std::function<void()> &work) { work(); }
  virtual std::optional<PersistedIdempotencyRecord>
  loadIdempotencyRecord(std::string_view scope, std::string_view key) = 0;
  virtual void
  saveIdempotencyRecord(const PersistedIdempotencyRecord &record) = 0;
  virtual void removeIdempotencyRecord(std::string_view scope,
                                       std::string_view key) = 0;
  virtual void cleanupIdempotencyRecords(std::int64_t now) = 0;
  virtual std::size_t idempotencyRecordCount() = 0;
};

class IdempotencyService final {
public:
  explicit IdempotencyService(IdempotencyPersistence *persistence = nullptr)
      : persistence_(persistence) {}

  IdempotencyCheck begin(std::string_view scope, std::string_view key,
                         std::string_view requestBody,
                         std::chrono::system_clock::time_point now,
                         bool permanent = false);
  bool complete(std::string_view scope, std::string_view key,
                std::string_view leaseToken, StoredHttpResult result,
                std::chrono::system_clock::time_point now);
  bool executeAndComplete(std::string_view scope, std::string_view key,
                          std::string_view leaseToken,
                          const std::function<StoredHttpResult()> &operation,
                          StoredHttpResult &result,
                          std::chrono::system_clock::time_point now);
  void abort(std::string_view scope, std::string_view key,
             std::string_view leaseToken);
  void cleanup(std::chrono::system_clock::time_point now);
  std::size_t size() const;

  static bool isUuid(std::string_view value);

private:
  struct Entry {
    std::string requestDigest;
    std::optional<StoredHttpResult> result;
    std::chrono::system_clock::time_point expiresAt;
    std::chrono::system_clock::time_point leaseExpiresAt;
    std::string leaseToken;
    bool permanent = false;
  };

  static std::string recordKey(std::string_view scope, std::string_view key);
  void cleanupUnlocked(std::chrono::system_clock::time_point now);
  void loadFromPersistenceUnlocked(std::string_view scope,
                                   std::string_view key);
  void persistUnlocked(std::string_view scope, std::string_view key,
                       const Entry &entry);

  mutable std::mutex mutex_;
  std::unordered_map<std::string, Entry> entries_;
  IdempotencyPersistence *persistence_ = nullptr;
  static constexpr std::size_t maximumEntries_ = 65536;
};

// RAII guard for an in-progress reservation: destroying the lease without a
// successful complete() aborts the reservation so a crashed or exception-path
// handler never leaves the key stuck for the next caller.
class IdempotencyLease final {
public:
  IdempotencyLease() = default;
  IdempotencyLease(IdempotencyService &service, std::string scope,
                   std::string key, std::string leaseToken);
  ~IdempotencyLease();

  IdempotencyLease(const IdempotencyLease &) = delete;
  IdempotencyLease &operator=(const IdempotencyLease &) = delete;
  IdempotencyLease(IdempotencyLease &&other) noexcept;
  IdempotencyLease &operator=(IdempotencyLease &&other) noexcept;

  bool valid() const;
  // Returns true when the stored reservation accepted the result; the guard
  // is disarmed only on success so a rejected complete() still aborts.
  bool complete(StoredHttpResult result,
                std::chrono::system_clock::time_point now);
  bool executeAndComplete(const std::function<StoredHttpResult()> &operation,
                          StoredHttpResult &result,
                          std::chrono::system_clock::time_point now);
  void abort();

private:
  IdempotencyService *service_ = nullptr;
  std::string scope_;
  std::string key_;
  std::string leaseToken_;
};

enum class VersionCheck { Match, Conflict, Invalid };
VersionCheck checkVersion(long long expectedVersion, long long currentVersion);

} // namespace ncs::core::application
