#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ncs::core::application {

struct AvatarData {
  std::vector<unsigned char> bytes;
  std::string contentType;
  std::string etag;
};

struct UserAccount {
  std::int64_t id = 0;
  std::string username;
  std::string phone;
  std::string nickname;
  std::optional<std::string> passwordHash;
  std::optional<AvatarData> avatar;
  int status = 1;
  std::int64_t registeredAt = 0;
  std::int64_t balanceCent = 0;
  std::int64_t debtCent = 0;
  bool hasActiveFlow = false;
  std::int64_t version = 1;
  bool deleted = false;
};

enum class AccountWriteResult {
  Success,
  NotFound,
  UsernameExists,
  PhoneExists,
  VersionConflict,
  ActiveFlowExists
};

// The wallet ledger owns the money values; the account row keeps a mirrored
// snapshot for profile responses and the active-flow flag used by deletion
// checks. Implementations must keep the mirror cheap and non-authoritative.
class WalletMirror {
public:
  virtual ~WalletMirror() = default;
  virtual void applyWalletState(std::int64_t userId, std::int64_t balanceCent,
                                std::int64_t debtCent) = 0;
  virtual void setActiveFlowFlag(std::int64_t userId, bool hasActiveFlow) = 0;
};

class UserAccountRepository {
public:
  virtual ~UserAccountRepository() = default;
  virtual std::optional<UserAccount> findById(std::int64_t id) const = 0;
  virtual std::optional<UserAccount>
  findByPhone(std::string_view phone) const = 0;
  virtual std::optional<UserAccount>
  findByLoginName(std::string_view loginName) const = 0;
  virtual AccountWriteResult create(UserAccount &account) = 0;
  virtual AccountWriteResult updateNickname(std::int64_t id,
                                            std::int64_t expectedVersion,
                                            std::string nickname,
                                            UserAccount &updated) = 0;
  virtual AccountWriteResult updateStatus(std::int64_t id, int status,
                                          UserAccount &updated) = 0;
  virtual AccountWriteResult updateCredential(std::int64_t id,
                                              std::string username,
                                              std::string passwordHash,
                                              UserAccount &updated) = 0;
  // Compare-and-swap on the stored digest: replaces it only when it still
  // matches expectedCurrentHash, so a background re-hash cannot clobber a
  // concurrent credential change. Does not bump the resource version.
  virtual AccountWriteResult
  replacePasswordHash(std::int64_t id, std::string_view expectedCurrentHash,
                      std::string_view newPasswordHash) = 0;
  virtual AccountWriteResult updateAvatar(std::int64_t id, AvatarData avatar,
                                          UserAccount &updated) = 0;
  virtual AccountWriteResult anonymize(std::int64_t id,
                                       UserAccount &updated) = 0;
  // Admin/ops surface: full account scan for management queries.
  virtual std::vector<UserAccount> listAccounts() = 0;
};

} // namespace ncs::core::application
