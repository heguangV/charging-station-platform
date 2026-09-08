// 统一 API 响应构造：success/code/message/userMessage/requestId 信封（契约见 docs/database-api.md
// §1.3）。 出口处对 message
// 做敏感信息过滤（SQL/token/路径/完整手机号等命中即替换为安全文案），并统一附加安全响应头。
#pragma once

#include "core/domain/error_code.h"

#include <crow.h>

#include <QJsonObject>

#include <string_view>

namespace ncs::server::controller
{

crow::response successResponse(QJsonObject data = {}, int httpStatus = 200,
                               std::string_view message = {}, std::string_view userMessage = {});
crow::response errorResponse(core::domain::ErrorCode code, std::string_view message,
                             std::string_view userMessage, QJsonObject data = {});
void applyPublicSecurityHeaders(crow::response& response);

} // namespace ncs::server::controller
