#pragma once

#include "core/domain/error_code.h"

#include <crow.h>

#include <QJsonObject>

#include <string_view>

namespace ncs::server::controller {

crow::response successResponse(
    QJsonObject data = {},
    int httpStatus = 200,
    std::string_view message = {},
    std::string_view userMessage = {});
crow::response errorResponse(
    core::domain::ErrorCode code,
    std::string_view message,
    std::string_view userMessage,
    QJsonObject data = {});
void applyPublicSecurityHeaders(crow::response &response);

} // namespace ncs::server::controller
