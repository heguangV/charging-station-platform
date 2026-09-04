#pragma once

#include "core/application/session_manager.h"
#include "core/application/user_account_repository.h"

#include <QJsonObject>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace ncs::core::application {
struct LoginResult;
}

namespace ncs::server::controller {

QJsonObject userJson(const core::application::UserAccount &user, bool includeAccountState);
QJsonObject loginJson(const core::application::LoginResult &login);
std::string maskedPhone(std::string_view phone);
std::int64_t unixSeconds(std::chrono::system_clock::time_point value);

struct ParsedAvatar {
    core::application::AvatarData avatar;
};

std::optional<ParsedAvatar> parseAndNormalizeAvatar(
    std::string_view contentType,
    const std::string &body);

} // namespace ncs::server::controller
