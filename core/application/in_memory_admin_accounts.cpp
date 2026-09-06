#include "core/application/in_memory_admin_repository.h"

#include <algorithm>

// UC-A-09 管理员账号管理的仓储操作。独立翻译单元存放，使
// in_memory_admin_repository.cpp 在按 .clang-format 重排后仍低于
// check.sh 的 700 行上限（NFR-M-01）。

namespace ncs::core::application
{

AdminAccount* InMemoryAdminRepository::findAdminByIdLocked(const std::int64_t id)
{
    for (auto& [username, admin] : admins_)
    {
        (void)username;
        if (admin.id == id)
            return &admin;
    }
    return nullptr;
}

AdminAccountPage InMemoryAdminRepository::adminAccounts(const AdminAccountQuery& query)
{
    std::lock_guard lock(mutex_);
    std::vector<const AdminAccount*> ordered;
    ordered.reserve(admins_.size());
    for (const auto& [username, admin] : admins_)
    {
        (void)username;
        ordered.push_back(&admin);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const AdminAccount* left, const AdminAccount* right)
              { return left->id > right->id; });
    AdminAccountPage page;
    page.total = static_cast<int>(ordered.size());
    page.page = query.page;
    page.pageSize = query.pageSize;
    const std::size_t first = static_cast<std::size_t>(query.page - 1) * query.pageSize;
    for (std::size_t index = first;
         index < ordered.size() && page.items.size() < static_cast<std::size_t>(query.pageSize);
         ++index)
    {
        page.items.push_back(*ordered[index]);
    }
    return page;
}

AdminAccountWriteResult InMemoryAdminRepository::createAdminAccount(
    const std::int64_t actorAdminId, const std::string_view username,
    const std::string_view passwordHash, const std::string_view reason, const std::int64_t at,
    AdminAccount& created)
{
    std::lock_guard lock(mutex_);
    if (admins_.count(std::string(username)) != 0)
        return AdminAccountWriteResult::UsernameExists;
    AdminAccount account;
    account.id = nextAdminId_++;
    account.username = std::string(username);
    account.passwordHash = std::string(passwordHash);
    account.status = 1;
    account.roles = {Role::Operator};
    account.mustChangePassword = true;
    account.version = 1;
    admins_.emplace(account.username, account);
    created = account;
    audit_.push_back(AuditEvent{actorAdminId, "ADMIN_CREATED", "ADMIN", std::to_string(account.id),
                                std::string(reason), at});
    return AdminAccountWriteResult::Success;
}

AdminAccountWriteResult InMemoryAdminRepository::updateAdminAccountStatus(
    const std::int64_t actorAdminId, const std::int64_t adminId, const int status,
    const std::string_view reason, const std::int64_t expectedVersion, const std::int64_t at,
    AdminAccount& updated)
{
    std::lock_guard lock(mutex_);
    AdminAccount* target = findAdminByIdLocked(adminId);
    if (!target)
        return AdminAccountWriteResult::NotFound;
    if (target->version != expectedVersion)
        return AdminAccountWriteResult::VersionConflict;
    target->status = status;
    ++target->version;
    updated = *target;
    audit_.push_back(AuditEvent{actorAdminId, status == 0 ? "ADMIN_DISABLED" : "ADMIN_ENABLED",
                                "ADMIN", std::to_string(adminId), std::string(reason), at});
    return AdminAccountWriteResult::Success;
}

AdminAccountWriteResult InMemoryAdminRepository::changeAdminAccountPassword(
    const std::int64_t actorAdminId, const std::int64_t adminId,
    const std::string_view expectedCurrentHash, const std::string_view newPasswordHash,
    const std::int64_t at, AdminAccount& updated)
{
    std::lock_guard lock(mutex_);
    AdminAccount* target = findAdminByIdLocked(adminId);
    if (!target)
        return AdminAccountWriteResult::NotFound;
    if (target->passwordHash != expectedCurrentHash)
        return AdminAccountWriteResult::HashMismatch;
    target->passwordHash = std::string(newPasswordHash);
    target->mustChangePassword = false;
    ++target->version;
    updated = *target;
    audit_.push_back(AuditEvent{
        actorAdminId, "ADMIN_PASSWORD_CHANGED", "ADMIN", std::to_string(adminId), {}, at});
    return AdminAccountWriteResult::Success;
}

} // namespace ncs::core::application
