#pragma once

#include "core/application/session_manager.h"
#include "core/domain/error_code.h"

#include <crow.h>

#include <chrono>
#include <optional>
#include <vector>

namespace ncs::server::middleware {

struct AuthorizationResult {
    std::optional<core::application::AuthContext> context;
    core::domain::ErrorCode error = core::domain::ErrorCode::Unauthorized;
};

AuthorizationResult authorize(
    const crow::request &request,
    core::application::SessionManager &sessions,
    const std::vector<core::application::TokenKind> &allowedTokenKinds,
    const std::vector<core::application::Role> &anyRequiredRole,
    std::chrono::system_clock::time_point now,
    bool requireRecentReauthentication = false);

} // namespace ncs::server::middleware
