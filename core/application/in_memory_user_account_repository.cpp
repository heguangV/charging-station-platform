#include "core/application/in_memory_user_account_repository.h"

#include "core/application/security_crypto.h"

#include <algorithm>

namespace ncs::core::application {

std::optional<UserAccount>
InMemoryUserAccountRepository::findById(const std::int64_t id) const {
  std::lock_guard lock(mutex_);
  const auto found = accounts_.find(id);
  return found == accounts_.end() || found->second.deleted
             ? std::nullopt
             : std::optional<UserAccount>(found->second);
}

std::optional<UserAccount>
InMemoryUserAccountRepository::findByPhone(const std::string_view phone) const {
  std::lock_guard lock(mutex_);
  for (const auto &[id, account] : accounts_) {
    if (!account.deleted && account.phone == phone)
      return account;
  }
  return std::nullopt;
}

std::optional<UserAccount> InMemoryUserAccountRepository::findByLoginName(
    const std::string_view loginName) const {
  std::lock_guard lock(mutex_);
  for (const auto &[id, account] : accounts_) {
    if (!account.deleted &&
        (account.username == loginName || account.phone == loginName)) {
      return account;
    }
  }
  return std::nullopt;
}

AccountWriteResult InMemoryUserAccountRepository::create(UserAccount &account) {
  std::lock_guard lock(mutex_);
  for (const auto &[id, existing] : accounts_) {
    if (!existing.deleted && existing.username == account.username) {
      return AccountWriteResult::UsernameExists;
    }
    if (!existing.deleted && existing.phone == account.phone) {
      return AccountWriteResult::PhoneExists;
    }
  }
  account.id = nextId_++;
  accounts_.emplace(account.id, account);
  return AccountWriteResult::Success;
}

AccountWriteResult InMemoryUserAccountRepository::updateNickname(
    const std::int64_t id, const std::int64_t expectedVersion,
    std::string nickname, UserAccount &updated) {
  std::lock_guard lock(mutex_);
  const auto found = accounts_.find(id);
  if (found == accounts_.end() || found->second.deleted)
    return AccountWriteResult::NotFound;
  if (found->second.version != expectedVersion)
    return AccountWriteResult::VersionConflict;
  found->second.nickname = std::move(nickname);
  ++found->second.version;
  updated = found->second;
  return AccountWriteResult::Success;
}

AccountWriteResult InMemoryUserAccountRepository::updateStatus(
    const std::int64_t id, const int status, UserAccount &updated) {
  std::lock_guard lock(mutex_);
  const auto found = accounts_.find(id);
  if (found == accounts_.end() || found->second.deleted)
    return AccountWriteResult::NotFound;
  found->second.status = status;
  ++found->second.version;
  updated = found->second;
  return AccountWriteResult::Success;
}

AccountWriteResult InMemoryUserAccountRepository::updateCredential(
    const std::int64_t id, std::string username, std::string passwordHash,
    UserAccount &updated) {
  std::lock_guard lock(mutex_);
  const auto found = accounts_.find(id);
  if (found == accounts_.end() || found->second.deleted)
    return AccountWriteResult::NotFound;
  for (const auto &[otherId, account] : accounts_) {
    if (otherId != id && !account.deleted && account.username == username) {
      return AccountWriteResult::UsernameExists;
    }
  }
  found->second.username = std::move(username);
  found->second.passwordHash = std::move(passwordHash);
  ++found->second.version;
  updated = found->second;
  return AccountWriteResult::Success;
}

AccountWriteResult InMemoryUserAccountRepository::replacePasswordHash(
    const std::int64_t id, const std::string_view expectedCurrentHash,
    const std::string_view newPasswordHash) {
  std::lock_guard lock(mutex_);
  const auto found = accounts_.find(id);
  if (found == accounts_.end() || found->second.deleted)
    return AccountWriteResult::NotFound;
  if (!found->second.passwordHash ||
      *found->second.passwordHash != expectedCurrentHash) {
    return AccountWriteResult::VersionConflict;
  }
  found->second.passwordHash = std::string(newPasswordHash);
  return AccountWriteResult::Success;
}

AccountWriteResult InMemoryUserAccountRepository::updateAvatar(
    const std::int64_t id, AvatarData avatar, UserAccount &updated) {
  std::lock_guard lock(mutex_);
  const auto found = accounts_.find(id);
  if (found == accounts_.end() || found->second.deleted)
    return AccountWriteResult::NotFound;
  found->second.avatar = std::move(avatar);
  ++found->second.version;
  updated = found->second;
  return AccountWriteResult::Success;
}

AccountWriteResult
InMemoryUserAccountRepository::anonymize(const std::int64_t id,
                                         UserAccount &updated) {
  std::lock_guard lock(mutex_);
  const auto found = accounts_.find(id);
  if (found == accounts_.end() || found->second.deleted)
    return AccountWriteResult::NotFound;
  if (found->second.hasActiveFlow)
    return AccountWriteResult::ActiveFlowExists;
  found->second.username =
      "deleted_" + std::to_string(id) + "_" + secureRandomToken(8);
  found->second.phone =
      "deleted_" + std::to_string(id) + "_" + secureRandomToken(8);
  found->second.nickname = "已注销用户";
  found->second.passwordHash.reset();
  found->second.avatar.reset();
  found->second.deleted = true;
  found->second.status = 0;
  ++found->second.version;
  updated = found->second;
  return AccountWriteResult::Success;
}

void InMemoryUserAccountRepository::applyWalletState(
    const std::int64_t userId, const std::int64_t balanceCent,
    const std::int64_t debtCent) {
  std::lock_guard lock(mutex_);
  const auto found = accounts_.find(userId);
  if (found == accounts_.end())
    return;
  found->second.balanceCent = balanceCent;
  found->second.debtCent = debtCent;
}

std::vector<UserAccount> InMemoryUserAccountRepository::listAccounts() {
  std::lock_guard lock(mutex_);
  std::vector<UserAccount> result;
  result.reserve(accounts_.size());
  for (const auto &[id, account] : accounts_) {
    (void)id;
    result.push_back(account);
  }
  return result;
}

void InMemoryUserAccountRepository::setActiveFlowFlag(
    const std::int64_t userId, const bool hasActiveFlow) {
  std::lock_guard lock(mutex_);
  const auto found = accounts_.find(userId);
  if (found == accounts_.end())
    return;
  found->second.hasActiveFlow = hasActiveFlow;
}

} // namespace ncs::core::application
