#pragma once

#include "core/application/session_manager.h"
#include "server/controller/api_response.h"
#include "server/middleware/authorization.h"

#include <crow.h>

#include <charconv>
#include <optional>
#include <string>

namespace ncs::server::controller {

// Shared guard for /api/v1/user routes: on success returns the user auth
// context, otherwise fills `failure` with the mapped error response.
inline std::optional<core::application::AuthContext> requireUser(
    const crow::request &request,
    core::application::SessionManager &sessions,
    crow::response &failure)
{
    const auto result = middleware::authorize(
        request, sessions,
        {core::application::TokenKind::User},
        {core::application::Role::User},
        std::chrono::system_clock::now());
    if (!result.context) {
        failure = errorResponse(
            result.error, "authentication failed", "账号或凭据错误");
        return std::nullopt;
    }
    return result.context;
}

inline std::optional<std::int64_t> principalIdAsInt(
    const core::application::AuthContext &context)
{
    constexpr std::string_view prefix = "user:";
    if (context.principalId.size() <= prefix.size()
        || std::string_view(context.principalId).substr(0, prefix.size()) != prefix) {
        return std::nullopt;
    }
    const std::string_view numeric(context.principalId.data() + prefix.size(),
                                   context.principalId.size() - prefix.size());
    std::int64_t id = 0;
    const auto [end, error] = std::from_chars(
        numeric.data(), numeric.data() + numeric.size(),
        id);
    if (error != std::errc{}
        || end != numeric.data() + numeric.size() || id < 1) {
        return std::nullopt;
    }
    return id;
}

// Guard for /api/v1/user business routes that address the wallet and flows by
// numeric user id; rejects tokens whose principal cannot be resolved.
inline std::optional<std::int64_t> requireUserId(
    const crow::request &request,
    core::application::SessionManager &sessions,
    crow::response &failure)
{
    const auto auth = requireUser(request, sessions, failure);
    if (!auth) return std::nullopt;
    const auto id = principalIdAsInt(*auth);
    if (!id) {
        failure = errorResponse(
            core::domain::ErrorCode::Unauthorized, "authentication failed", "账号或凭据错误");
        return std::nullopt;
    }
    return id;
}

} // namespace ncs::server::controller
