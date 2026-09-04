#include "core/application/idempotency_service.h"

#include "core/application/security_crypto.h"

#include <cctype>

namespace ncs::core::application {
namespace {

std::int64_t unixSeconds(const std::chrono::system_clock::time_point value) {
  return std::chrono::duration_cast<std::chrono::seconds>(
             value.time_since_epoch())
      .count();
}

std::chrono::system_clock::time_point
fromUnixSeconds(const std::int64_t value) {
  return std::chrono::system_clock::time_point(std::chrono::seconds(value));
}

} // namespace

IdempotencyCheck IdempotencyService::begin(
    const std::string_view scope, const std::string_view key,
    const std::string_view requestBody,
    const std::chrono::system_clock::time_point now, const bool permanent) {
  if (scope.empty() || !isUuid(key))
    return {};
  const std::string combined = recordKey(scope, key);
  const std::string digest = sha256Hex(requestBody);
  std::lock_guard lock(mutex_);
  loadFromPersistenceUnlocked(scope, key);
  const auto found = entries_.find(combined);
  if (found != entries_.end() &&
      ((!found->second.result && found->second.leaseExpiresAt <= now) ||
       (found->second.result && !found->second.permanent &&
        found->second.expiresAt <= now))) {
    entries_.erase(found);
    if (persistence_)
      persistence_->removeIdempotencyRecord(scope, key);
  }
  const auto active = entries_.find(combined);
  if (active == entries_.end()) {
    const std::size_t storedSize =
        persistence_ ? persistence_->idempotencyRecordCount() : entries_.size();
    if (storedSize >= maximumEntries_) {
      cleanupUnlocked(now);
      if (persistence_)
        persistence_->cleanupIdempotencyRecords(unixSeconds(now));
      if ((persistence_ ? persistence_->idempotencyRecordCount()
                        : entries_.size()) >= maximumEntries_) {
        return {IdempotencyDecision::CapacityExceeded, std::nullopt,
                std::nullopt};
      }
    }
    const std::string leaseToken = secureRandomToken(16);
    const auto inserted =
        entries_.emplace(combined, Entry{
                                       digest,
                                       std::nullopt,
                                       {},
                                       now + std::chrono::minutes(10),
                                       leaseToken,
                                       permanent,
                                   });
    try {
      persistUnlocked(scope, key, inserted.first->second);
    } catch (...) {
      entries_.erase(inserted.first);
      throw;
    }
    return {IdempotencyDecision::Proceed, std::nullopt, leaseToken};
  }
  if (active->second.requestDigest != digest) {
    return {IdempotencyDecision::Conflict, std::nullopt};
  }
  if (!active->second.result) {
    return {IdempotencyDecision::InProgress, std::nullopt};
  }
  return {IdempotencyDecision::Replay, active->second.result};
}

bool IdempotencyService::complete(
    const std::string_view scope, const std::string_view key,
    const std::string_view leaseToken, StoredHttpResult result,
    const std::chrono::system_clock::time_point now) {
  std::lock_guard lock(mutex_);
  const auto found = entries_.find(recordKey(scope, key));
  if (found == entries_.end() || found->second.result.has_value() ||
      found->second.leaseToken != leaseToken ||
      found->second.leaseExpiresAt <= now)
    return false;
  // Only a successful result (2xx) is stored. Client/business errors (4xx)
  // produced no side effects and may legitimately change between retries
  // (e.g. VersionConflict after the client refreshes the resource version),
  // so the same key must re-execute instead of replaying a stale refusal
  // forever; a 5xx is an infrastructure failure that must never be replayed
  // either. Both paths release the key so the documented same-key retry
  // re-executes the operation.
  if (result.status >= 400) {
    if (persistence_)
      persistence_->removeIdempotencyRecord(scope, key);
    entries_.erase(found);
    return true;
  }
  Entry completed = found->second;
  completed.result = std::move(result);
  completed.leaseToken.clear();
  if (!completed.permanent)
    completed.expiresAt = now + std::chrono::hours(24 * 7);
  persistUnlocked(scope, key, completed);
  found->second = std::move(completed);
  return true;
}

bool IdempotencyService::executeAndComplete(
    const std::string_view scope, const std::string_view key,
    const std::string_view leaseToken,
    const std::function<StoredHttpResult()> &operation,
    StoredHttpResult &result, const std::chrono::system_clock::time_point now) {
  if (!operation)
    return false;
  if (!persistence_) {
    result = operation();
    if (result.status >= 400) {
      abort(scope, key, leaseToken);
      return true;
    }
    return complete(scope, key, leaseToken, result, now);
  }

  const std::string combined = recordKey(scope, key);
  Entry completed;
  {
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(combined);
    if (found == entries_.end() || found->second.result.has_value() ||
        found->second.leaseToken != leaseToken ||
        found->second.leaseExpiresAt <= now) {
      return false;
    }
    completed = found->second;
  }

  StoredHttpResult produced;
  persistence_->withTransaction([&] {
    produced = operation();
    if (produced.status >= 400) {
      // Commit any deliberately persisted recovery state from the operation,
      // but release the key so the documented same-key retry can recover it.
      persistence_->removeIdempotencyRecord(scope, key);
    } else {
      completed.result = produced;
      completed.leaseToken.clear();
      if (!completed.permanent)
        completed.expiresAt = now + std::chrono::hours(24 * 7);
      persistence_->saveIdempotencyRecord(PersistedIdempotencyRecord{
          std::string(scope),
          std::string(key),
          completed.requestDigest,
          completed.result,
          unixSeconds(completed.expiresAt),
          unixSeconds(completed.leaseExpiresAt),
          completed.leaseToken,
          completed.permanent,
      });
    }
  });

  {
    std::lock_guard lock(mutex_);
    if (produced.status >= 400) {
      const auto found = entries_.find(combined);
      if (found != entries_.end() && !found->second.result &&
          found->second.leaseToken == leaseToken) {
        entries_.erase(found);
      }
      result = std::move(produced);
      return true;
    }
    const auto found = entries_.find(combined);
    if (found == entries_.end() || found->second.result.has_value() ||
        found->second.leaseToken != leaseToken) {
      // The database is authoritative after commit; refresh a stale cache so
      // subsequent callers replay instead of re-executing the operation.
      entries_[combined] = completed;
    } else {
      found->second = completed;
    }
  }
  result = std::move(produced);
  return true;
}

void IdempotencyService::abort(const std::string_view scope,
                               const std::string_view key,
                               const std::string_view leaseToken) {
  std::lock_guard lock(mutex_);
  const auto found = entries_.find(recordKey(scope, key));
  if (found != entries_.end() && !found->second.result &&
      found->second.leaseToken == leaseToken) {
    entries_.erase(found);
    if (persistence_) {
      try {
        persistence_->removeIdempotencyRecord(scope, key);
      } catch (...) {
        // abort() is called by IdempotencyLease's destructor and must
        // never terminate the process. The persisted lease has a
        // short expiry and can be reclaimed safely.
      }
    }
  }
}

void IdempotencyService::cleanup(
    const std::chrono::system_clock::time_point now) {
  std::lock_guard lock(mutex_);
  cleanupUnlocked(now);
  if (persistence_)
    persistence_->cleanupIdempotencyRecords(unixSeconds(now));
}

std::size_t IdempotencyService::size() const {
  std::lock_guard lock(mutex_);
  return persistence_ ? persistence_->idempotencyRecordCount()
                      : entries_.size();
}

void IdempotencyService::loadFromPersistenceUnlocked(
    const std::string_view scope, const std::string_view key) {
  if (!persistence_)
    return;
  const std::string combined = recordKey(scope, key);
  if (entries_.find(combined) != entries_.end())
    return;
  const auto stored = persistence_->loadIdempotencyRecord(scope, key);
  if (!stored)
    return;
  entries_.emplace(combined, Entry{
                                 stored->requestDigest,
                                 stored->result,
                                 fromUnixSeconds(stored->expiresAt),
                                 fromUnixSeconds(stored->leaseExpiresAt),
                                 stored->leaseToken,
                                 stored->permanent,
                             });
}

void IdempotencyService::persistUnlocked(const std::string_view scope,
                                         const std::string_view key,
                                         const Entry &entry) {
  if (!persistence_)
    return;
  persistence_->saveIdempotencyRecord(PersistedIdempotencyRecord{
      std::string(scope),
      std::string(key),
      entry.requestDigest,
      entry.result,
      unixSeconds(entry.expiresAt),
      unixSeconds(entry.leaseExpiresAt),
      entry.leaseToken,
      entry.permanent,
  });
}

void IdempotencyService::cleanupUnlocked(
    const std::chrono::system_clock::time_point now) {
  for (auto iterator = entries_.begin(); iterator != entries_.end();) {
    if ((!iterator->second.result && iterator->second.leaseExpiresAt <= now) ||
        (iterator->second.result && !iterator->second.permanent &&
         iterator->second.expiresAt <= now)) {
      iterator = entries_.erase(iterator);
    } else {
      ++iterator;
    }
  }
}

IdempotencyLease::IdempotencyLease(IdempotencyService &service,
                                   std::string scope, std::string key,
                                   std::string leaseToken)
    : service_(&service), scope_(std::move(scope)), key_(std::move(key)),
      leaseToken_(std::move(leaseToken)) {}

IdempotencyLease::~IdempotencyLease() {
  if (service_)
    service_->abort(scope_, key_, leaseToken_);
}

IdempotencyLease::IdempotencyLease(IdempotencyLease &&other) noexcept
    : service_(other.service_), scope_(std::move(other.scope_)),
      key_(std::move(other.key_)), leaseToken_(std::move(other.leaseToken_)) {
  other.service_ = nullptr;
}

IdempotencyLease &
IdempotencyLease::operator=(IdempotencyLease &&other) noexcept {
  if (this != &other) {
    if (service_)
      service_->abort(scope_, key_, leaseToken_);
    service_ = other.service_;
    scope_ = std::move(other.scope_);
    key_ = std::move(other.key_);
    leaseToken_ = std::move(other.leaseToken_);
    other.service_ = nullptr;
  }
  return *this;
}

bool IdempotencyLease::valid() const { return service_ != nullptr; }

bool IdempotencyLease::complete(StoredHttpResult result,
                                std::chrono::system_clock::time_point now) {
  if (!service_)
    return false;
  const bool completed =
      service_->complete(scope_, key_, leaseToken_, std::move(result), now);
  if (completed)
    service_ = nullptr;
  return completed;
}

bool IdempotencyLease::executeAndComplete(
    const std::function<StoredHttpResult()> &operation,
    StoredHttpResult &result, const std::chrono::system_clock::time_point now) {
  if (!service_)
    return false;
  const bool completed = service_->executeAndComplete(scope_, key_, leaseToken_,
                                                      operation, result, now);
  if (completed)
    service_ = nullptr;
  return completed;
}

void IdempotencyLease::abort() {
  if (!service_)
    return;
  service_->abort(scope_, key_, leaseToken_);
  service_ = nullptr;
}

bool IdempotencyService::isUuid(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-') {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23)
      continue;
    if (!std::isxdigit(static_cast<unsigned char>(value[index])))
      return false;
  }
  return true;
}

std::string IdempotencyService::recordKey(const std::string_view scope,
                                          const std::string_view key) {
  return std::string(scope) + "\n" + std::string(key);
}

VersionCheck checkVersion(const long long expectedVersion,
                          const long long currentVersion) {
  if (expectedVersion < 1 || currentVersion < 1)
    return VersionCheck::Invalid;
  return expectedVersion == currentVersion ? VersionCheck::Match
                                           : VersionCheck::Conflict;
}

} // namespace ncs::core::application
