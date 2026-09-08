#include "core/application/user_account_repository.h"
#include "infrastructure/sqlite/sqlite_repository.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace
{

std::int64_t processId()
{
#if defined(_WIN32)
    return static_cast<std::int64_t>(::_getpid());
#else
    return static_cast<std::int64_t>(::getpid());
#endif
}

class TemporaryDatabase final
{
  public:
    TemporaryDatabase()
        : path_(std::filesystem::temp_directory_path() /
                ("ncs-avatar-sqlite-" + std::to_string(processId()) + ".db"))
    {
        cleanup();
    }

    ~TemporaryDatabase()
    {
        cleanup();
    }

    std::string path() const
    {
        return path_.string();
    }

  private:
    void cleanup() const
    {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(path_.string() + "-wal", ignored);
        std::filesystem::remove(path_.string() + "-shm", ignored);
    }

    std::filesystem::path path_;
};

class TestRunner final
{
  public:
    void check(const bool condition, const std::string_view message)
    {
        if (condition)
            return;
        std::cerr << "FAIL: " << message << '\n';
        ++failures_;
    }

    int result() const
    {
        return failures_ == 0 ? 0 : 1;
    }

  private:
    int failures_ = 0;
};

} // namespace

int main()
{
    using namespace ncs::core::application;
    using ncs::infrastructure::sqlite::SqliteRepository;

    TestRunner tests;
    TemporaryDatabase database;
    std::int64_t userId = 0;
    const AvatarData avatar{{0x89, 0x50, 0x4e, 0x47}, "image/png", "\"avatar-etag\""};

    {
        SqliteRepository repository(database.path());
        UserAccount account;
        account.username = "avatar_user";
        account.phone = "13800139998";
        account.nickname = "头像用户";
        account.registeredAt = 1788500000;
        tests.check(repository.create(account) == AccountWriteResult::Success,
                    "avatar account is created");
        userId = account.id;

        UserAccount updated;
        tests.check(repository.updateAvatar(userId, avatar, updated) ==
                            AccountWriteResult::Success &&
                        updated.avatar && updated.avatar->bytes == avatar.bytes &&
                        updated.avatar->contentType == avatar.contentType &&
                        updated.avatar->etag == avatar.etag && updated.version == 2,
                    "avatar and account version are written atomically");
    }

    {
        SqliteRepository repository(database.path());
        const auto persisted = repository.findById(userId);
        tests.check(persisted && persisted->avatar && persisted->avatar->bytes == avatar.bytes &&
                        persisted->avatar->etag == avatar.etag,
                    "avatar survives repository restart");

        UserAccount anonymized;
        tests.check(repository.anonymize(userId, anonymized) == AccountWriteResult::Success,
                    "account anonymization deletes its avatar");
        tests.check(repository.updateAvatar(userId, avatar, anonymized) ==
                            AccountWriteResult::NotFound &&
                        !repository.findById(userId),
                    "anonymized account cannot recreate an avatar");
    }

    return tests.result();
}
