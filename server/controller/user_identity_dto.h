// 用户身份模块 DTO 组装：UserAccount/LoginResult → JSON（手机号固定脱敏为 138****5678
// 形式，时间统一转 Unix 秒）。 另含头像上传的解析与规格化（parseAndNormalizeAvatar），供
// user_identity_routes 组装响应与入库数据。
#pragma once

#include "core/application/session_manager.h"
#include "core/application/user_account_repository.h"

#include <QJsonObject>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace ncs::core::application
{
struct LoginResult;
}

namespace ncs::server::controller
{

QJsonObject userJson(const core::application::UserAccount& user, bool includeAccountState);
QJsonObject loginJson(const core::application::LoginResult& login);
std::string maskedPhone(std::string_view phone);
std::int64_t unixSeconds(std::chrono::system_clock::time_point value);

struct ParsedAvatar
{
    core::application::AvatarData avatar;
};

std::optional<ParsedAvatar> parseAndNormalizeAvatar(std::string_view contentType,
                                                    const std::string& body);

} // namespace ncs::server::controller
