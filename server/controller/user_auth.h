// 用户端接口鉴权守卫：requireUser 校验 User 令牌并把失败映射为统一错误响应；requireUserId 再把
// principal 解析为数字用户 ID。 /api/v1/user 业务路由（钱包、充电流程等）共用的身份第一道防线。
#pragma once

#include "core/application/session_manager.h"
#include "server/controller/api_response.h"
#include "server/middleware/authorization.h"

#include <crow.h>

#include <charconv>
#include <optional>
#include <string>

namespace ncs::server::controller
{

// Shared guard for /api/v1/user routes: on success returns the user auth
// context, otherwise fills `failure` with the mapped error response.
inline std::optional<core::application::AuthContext>
requireUser(const crow::request& request, core::application::SessionManager& sessions,
            crow::response& failure)
{
    const auto result =
        middleware::authorize(request, sessions, {core::application::TokenKind::User},
                              {core::application::Role::User}, std::chrono::system_clock::now());
    if (!result.context)
    {
        failure = errorResponse(result.error, "authentication failed", "账号或凭据错误");
        return std::nullopt;
    }
    return result.context;
}

// principalId 形如 "user:<id>"：解析为 int64，格式不合法即视为无法寻址，调用方按未授权拒绝。
inline std::optional<std::int64_t> principalIdAsInt(const core::application::AuthContext& context)
{
    constexpr std::string_view prefix = "user:";
    if (context.principalId.size() <= prefix.size() ||
        std::string_view(context.principalId).substr(0, prefix.size()) != prefix)
    {
        return std::nullopt;
    }
    const std::string_view numeric(context.principalId.data() + prefix.size(),
                                   context.principalId.size() - prefix.size());
    std::int64_t id = 0;
    const auto [end, error] = std::from_chars(numeric.data(), numeric.data() + numeric.size(), id);
    if (error != std::errc{} || end != numeric.data() + numeric.size() || id < 1)
    {
        return std::nullopt;
    }
    return id;
}

// Guard for /api/v1/user business routes that address the wallet and flows by
// numeric user id; rejects tokens whose principal cannot be resolved.
inline std::optional<std::int64_t> requireUserId(const crow::request& request,
                                                 core::application::SessionManager& sessions,
                                                 crow::response& failure)
{
    const auto auth = requireUser(request, sessions, failure);
    if (!auth)
        return std::nullopt;
    const auto id = principalIdAsInt(*auth);
    if (!id)
    {
        failure = errorResponse(core::domain::ErrorCode::Unauthorized, "authentication failed",
                                "账号或凭据错误");
        return std::nullopt;
    }
    return id;
}

} // namespace ncs::server::controller
