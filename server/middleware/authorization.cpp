#include "server/middleware/authorization.h"

#include <algorithm>

namespace ncs::server::middleware
{

// 统一鉴权入口：解析 Bearer 令牌并经会话管理器认证，依次校验令牌类型与访问路径的匹配、
// 允许的令牌种类与角色（任一满足即可），必要时要求近期已完成重新认证；失败返回空上下文
// 并携带 Forbidden / ReauthRequired 错误码。
AuthorizationResult authorize(const crow::request& request,
                              core::application::SessionManager& sessions,
                              const std::vector<core::application::TokenKind>& allowedTokenKinds,
                              const std::vector<core::application::Role>& anyRequiredRole,
                              const std::chrono::system_clock::time_point now,
                              const bool requireRecentReauthentication)
{
    const auto bearer =
        core::application::SessionManager::parseBearer(request.get_header_value("Authorization"));
    if (!bearer)
        return {};
    auto context = sessions.authenticate(*bearer, now);
    if (!context)
        return {};
    if (!core::application::SessionManager::allowsPath(context->tokenKind, request.url) ||
        std::find(allowedTokenKinds.begin(), allowedTokenKinds.end(), context->tokenKind) ==
            allowedTokenKinds.end())
    {
        return {std::nullopt, core::domain::ErrorCode::Forbidden};
    }
    if (!anyRequiredRole.empty() &&
        std::none_of(anyRequiredRole.begin(), anyRequiredRole.end(),
                     [&context](const auto role)
                     { return core::application::SessionManager::hasRole(*context, role); }))
    {
        return {std::nullopt, core::domain::ErrorCode::Forbidden};
    }
    if (requireRecentReauthentication && !sessions.hasRecentReauthentication(*context, now))
    {
        return {std::nullopt, core::domain::ErrorCode::ReauthRequired};
    }
    return {std::move(context), core::domain::ErrorCode::Ok};
}

} // namespace ncs::server::middleware
