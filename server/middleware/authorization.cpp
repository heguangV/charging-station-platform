#include "server/middleware/authorization.h"

#include <algorithm>

namespace ncs::server::middleware {

AuthorizationResult authorize(
    const crow::request &request,
    core::application::SessionManager &sessions,
    const std::vector<core::application::TokenKind> &allowedTokenKinds,
    const std::vector<core::application::Role> &anyRequiredRole,
    const std::chrono::system_clock::time_point now,
    const bool requireRecentReauthentication)
{
    const auto bearer = core::application::SessionManager::parseBearer(
        request.get_header_value("Authorization"));
    if (!bearer) return {};
    auto context = sessions.authenticate(*bearer, now);
    if (!context) return {};
    if (!core::application::SessionManager::allowsPath(context->tokenKind, request.url)
        || std::find(allowedTokenKinds.begin(), allowedTokenKinds.end(), context->tokenKind)
            == allowedTokenKinds.end()) {
        return {std::nullopt, core::domain::ErrorCode::Forbidden};
    }
    if (!anyRequiredRole.empty()
        && std::none_of(
            anyRequiredRole.begin(), anyRequiredRole.end(),
            [&context](const auto role) {
                return core::application::SessionManager::hasRole(*context, role);
            })) {
        return {std::nullopt, core::domain::ErrorCode::Forbidden};
    }
    if (requireRecentReauthentication
        && !sessions.hasRecentReauthentication(*context, now)) {
        return {std::nullopt, core::domain::ErrorCode::ReauthRequired};
    }
    return {std::move(context), core::domain::ErrorCode::Ok};
}

} // namespace ncs::server::middleware
