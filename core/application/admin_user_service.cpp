#include "core/application/admin_user_service.h"

#include <cctype>

namespace ncs::core::application {
namespace {

std::int64_t unixSeconds(const std::chrono::system_clock::time_point value) {
  return std::chrono::duration_cast<std::chrono::seconds>(
             value.time_since_epoch())
      .count();
}

} // namespace

AdminUserPage AdminUserService::list(const AdminUserQuery &query) {
  return repository_.listManagedUsers(query);
}

ServiceResult<AdminUserDetail>
AdminUserService::detail(const std::int64_t userId,
                         const std::chrono::system_clock::time_point now) {
  const auto user = repository_.findManagedUser(userId);
  if (!user)
    return {core::domain::ErrorCode::NotFound, std::nullopt};
  AdminUserDetail detail;
  detail.user = *user;
  detail.activeSessionCount =
      sessions_.activeSessions(userPrincipal(userId), now).size();
  detail.activeFlow = flows_.activeFlow(userId, now);
  return {core::domain::ErrorCode::Ok, std::move(detail)};
}

ServiceResult<AdminStatusUpdate> AdminUserService::updateStatus(
    const std::int64_t actorAdminId, const std::int64_t userId,
    const int status, std::string reason, const std::int64_t expectedVersion,
    const std::chrono::system_clock::time_point now) {
  if ((status != 0 && status != 1) || expectedVersion < 1 ||
      !validReason(reason)) {
    return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
  }
  UserAccount updated;
  AccountWriteResult write = AccountWriteResult::NotFound;
  repository_.withTransaction([&] {
    write = repository_.updateManagedUserStatus(
        actorAdminId, userId, status, reason, expectedVersion, unixSeconds(now),
        updated);
  });
  if (write == AccountWriteResult::NotFound)
    return {core::domain::ErrorCode::NotFound, std::nullopt};
  if (write == AccountWriteResult::VersionConflict)
    return {core::domain::ErrorCode::VersionConflict, std::nullopt};
  const bool activeFlow = flows_.activeFlow(userId, now).hasActiveFlow;
  if (status == 0)
    sessions_.revokePrincipal(userPrincipal(userId));
  return {core::domain::ErrorCode::Ok,
          AdminStatusUpdate{std::move(updated), status == 0 && activeFlow}};
}

ServiceResult<OrderPage> AdminUserService::orders(
    const std::int64_t actorAdminId, const std::int64_t userId,
    const std::optional<int> status, const std::int64_t fromAt,
    const std::int64_t toAt, const std::string &sort, const int page,
    const int pageSize, const std::chrono::system_clock::time_point now) {
  if (!repository_.findManagedUser(userId))
    return {core::domain::ErrorCode::NotFound, std::nullopt};
  ServiceResult<OrderPage> result;
  repository_.withTransaction([&] {
    result = flows_.orders(userId, status, fromAt, toAt, sort, page, pageSize);
    if (result.ok()) {
      repository_.addAuditEvent(AuditEvent{
          actorAdminId, "USER_ORDERS_VIEWED", "USER", std::to_string(userId),
          {}, unixSeconds(now)});
    }
  });
  return result;
}

bool AdminUserService::validReason(const std::string_view reason) {
  if (reason.size() < 2 || reason.size() > 200)
    return false;
  for (const unsigned char character : reason) {
    if (character < 0x20 || character == 0x7f)
      return false;
  }
  return true;
}

std::string AdminUserService::userPrincipal(const std::int64_t userId) {
  return "user:" + std::to_string(userId);
}

} // namespace ncs::core::application
