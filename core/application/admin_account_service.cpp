#include "core/application/admin_account_service.h"

#include <cctype>

namespace ncs::core::application
{
namespace
{

std::int64_t unixSeconds(const std::chrono::system_clock::time_point value)
{
    return std::chrono::duration_cast<std::chrono::seconds>(value.time_since_epoch()).count();
}

bool isUsernameCharacter(const char value)
{
    const auto character = static_cast<unsigned char>(value);
    return std::isalnum(character) != 0 || character == '_';
}

} // namespace

AdminAccountPage AdminAccountService::list(const AdminAccountQuery& query)
{
    return repository_.adminAccounts(query);
}

ServiceResult<AdminAccount>
AdminAccountService::create(const std::int64_t actorAdminId, std::string username,
                            std::string password, std::string reason,
                            const std::chrono::system_clock::time_point now)
{
    if (!validUsername(username) || password.size() < 10 || password.size() > 128 ||
        !validReason(reason))
    {
        return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
    }
    AdminAccount created;
    AdminAccountWriteResult write = AdminAccountWriteResult::NotFound;
    repository_.withTransaction(
        [&]
        {
            write = repository_.createAdminAccount(actorAdminId, username,
                                                   passwordHasher_.hash(password), reason,
                                                   unixSeconds(now), created);
        });
    if (write == AdminAccountWriteResult::UsernameExists)
        return {core::domain::ErrorCode::AlreadyExists, std::nullopt};
    if (write != AdminAccountWriteResult::Success)
        return {core::domain::ErrorCode::InternalError, std::nullopt};
    return {core::domain::ErrorCode::Ok, std::move(created)};
}

ServiceResult<AdminAccount>
AdminAccountService::updateStatus(const std::int64_t actorAdminId, const std::int64_t adminId,
                                  const int status, std::string reason,
                                  const std::int64_t expectedVersion,
                                  const std::chrono::system_clock::time_point now)
{
    if ((status != 0 && status != 1) || expectedVersion < 1 || !validReason(reason))
    {
        return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
    }
    if (status == 0 && adminId == actorAdminId)
    {
        // SRS UC-A-09: an OWNER must not disable the account backing the current
        // privileged session; another OWNER stays able to manage it.
        return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
    }
    AdminAccount updated;
    AdminAccountWriteResult write = AdminAccountWriteResult::NotFound;
    repository_.withTransaction(
        [&]
        {
            write = repository_.updateAdminAccountStatus(
                actorAdminId, adminId, status, reason, expectedVersion, unixSeconds(now), updated);
        });
    if (write == AdminAccountWriteResult::NotFound)
        return {core::domain::ErrorCode::NotFound, std::nullopt};
    if (write == AdminAccountWriteResult::VersionConflict)
        return {core::domain::ErrorCode::VersionConflict, std::nullopt};
    if (write != AdminAccountWriteResult::Success)
        return {core::domain::ErrorCode::InternalError, std::nullopt};
    if (status == 0)
        sessions_.revokePrincipal(adminPrincipal(adminId));
    return {core::domain::ErrorCode::Ok, std::move(updated)};
}

ServiceResult<AdminAccount> AdminAccountService::changeOwnPassword(
    const std::int64_t actorAdminId, const std::int64_t sessionId, std::string currentPassword,
    std::string newPassword, const std::chrono::system_clock::time_point now)
{
    if (newPassword.size() < 10 || newPassword.size() > 128)
    {
        return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
    }
    const auto current = repository_.findAdminById(actorAdminId);
    if (!current)
        return {core::domain::ErrorCode::NotFound, std::nullopt};
    if (!passwordHasher_.verify(currentPassword, current->passwordHash))
        return {core::domain::ErrorCode::Unauthorized, std::nullopt};
    AdminAccount updated;
    AdminAccountWriteResult write = AdminAccountWriteResult::NotFound;
    repository_.withTransaction(
        [&]
        {
            write = repository_.changeAdminAccountPassword(
                actorAdminId, actorAdminId, current->passwordHash,
                passwordHasher_.hash(newPassword), unixSeconds(now), updated);
        });
    if (write == AdminAccountWriteResult::NotFound)
        return {core::domain::ErrorCode::NotFound, std::nullopt};
    if (write == AdminAccountWriteResult::HashMismatch)
        return {core::domain::ErrorCode::Unauthorized, std::nullopt};
    if (write != AdminAccountWriteResult::Success)
        return {core::domain::ErrorCode::InternalError, std::nullopt};
    sessions_.revokeOtherSessions(adminPrincipal(actorAdminId), sessionId);
    return {core::domain::ErrorCode::Ok, std::move(updated)};
}

bool AdminAccountService::validUsername(const std::string_view username)
{
    if (username.size() < 3 || username.size() > 32)
        return false;
    for (const char character : username)
    {
        if (!isUsernameCharacter(character))
            return false;
    }
    return true;
}

bool AdminAccountService::validReason(const std::string_view reason)
{
    if (reason.size() < 2 || reason.size() > 200)
        return false;
    for (const unsigned char character : reason)
    {
        if (character < 0x20 || character == 0x7f)
            return false;
    }
    return true;
}

std::string AdminAccountService::adminPrincipal(const std::int64_t adminId)
{
    return "admin:" + std::to_string(adminId);
}

} // namespace ncs::core::application
