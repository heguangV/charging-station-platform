// 请求鉴权核心：从 Authorization 头取 Bearer 令牌，经 SessionManager
// 认证并校验令牌类型（User/Admin/MlTask）与所需角色。
// 可要求近期重新认证（requireRecentReauthentication，用于敏感管理操作）；失败返回统一
// ErrorCode，由路由层映射错误响应。
#pragma once

#include "core/application/session_manager.h"
#include "core/domain/error_code.h"

#include <crow.h>

#include <chrono>
#include <optional>
#include <vector>

namespace ncs::server::middleware
{

struct AuthorizationResult
{
    std::optional<core::application::AuthContext> context;
    core::domain::ErrorCode error = core::domain::ErrorCode::Unauthorized;
};

AuthorizationResult authorize(const crow::request& request,
                              core::application::SessionManager& sessions,
                              const std::vector<core::application::TokenKind>& allowedTokenKinds,
                              const std::vector<core::application::Role>& anyRequiredRole,
                              std::chrono::system_clock::time_point now,
                              bool requireRecentReauthentication = false);

} // namespace ncs::server::middleware
