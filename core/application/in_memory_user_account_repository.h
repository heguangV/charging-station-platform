#pragma once

#include "core/application/user_account_repository.h"

#include <mutex>
#include <unordered_map>

namespace ncs::core::application {

class InMemoryUserAccountRepository final : public UserAccountRepository,
                                            public WalletMirror {
public:
  std::optional<UserAccount> findById(std::int64_t id) const override;
  std::optional<UserAccount> findByPhone(std::string_view phone) const override;
  std::optional<UserAccount>
  findByLoginName(std::string_view loginName) const override;
  AccountWriteResult create(UserAccount &account) override;
  AccountWriteResult updateNickname(std::int64_t id,
                                    std::int64_t expectedVersion,
                                    std::string nickname,
                                    UserAccount &updated) override;
  AccountWriteResult updateStatus(std::int64_t id, int status,
                                  UserAccount &updated) override;
  AccountWriteResult updateCredential(std::int64_t id, std::string username,
                                      std::string passwordHash,
                                      UserAccount &updated) override;
  AccountWriteResult
  replacePasswordHash(std::int64_t id, std::string_view expectedCurrentHash,
                      std::string_view newPasswordHash) override;
  AccountWriteResult updateAvatar(std::int64_t id, AvatarData avatar,
                                  UserAccount &updated) override;
  AccountWriteResult anonymize(std::int64_t id, UserAccount &updated) override;

  void applyWalletState(std::int64_t userId, std::int64_t balanceCent,
                        std::int64_t debtCent) override;
  void setActiveFlowFlag(std::int64_t userId, bool hasActiveFlow) override;

  std::vector<UserAccount> listAccounts() override;

private:
  mutable std::mutex mutex_;
  std::unordered_map<std::int64_t, UserAccount> accounts_;
  std::int64_t nextId_ = 1;
};

} // namespace ncs::core::application
