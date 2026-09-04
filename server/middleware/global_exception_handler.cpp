#include "server/middleware/global_exception_handler.h"

#include "core/domain/error_code.h"
#include "infrastructure/files/structured_logger.h"
#include "server/controller/api_response.h"

#include <string_view>

namespace ncs::server::middleware {
namespace {

using infrastructure::files::LogLevel;
using infrastructure::files::StructuredLogger;

constexpr std::string_view fallbackBody =
    R"({"code":13,"data":null,"message":"internal error","requestId":"","success":false,"userMessage":"服务暂时不可用，请稍后重试"})";

void setSafeResponse(crow::response &response, std::string body)
{
    response.clear();
    response.code = 500;
    response.body = std::move(body);
    response.set_header("Content-Type", "application/json; charset=utf-8");
    response.set_header("Cache-Control", "no-store");
}

} // namespace

void installGlobalExceptionHandler(
    ServerApp &application,
    StructuredLogger &logger)
{
    application.exception_handler([&logger](crow::response &response) {
        try {
            logger.log(
                LogLevel::Error,
                "server.exception",
                "unhandled request exception");
        } catch (...) {
            // Error handling must remain available when the log sink is unavailable.
        }

        try {
            response = controller::errorResponse(
                core::domain::ErrorCode::InternalError,
                "internal error",
                "服务暂时不可用，请稍后重试");
        } catch (...) {
            setSafeResponse(response, std::string(fallbackBody));
        }
    });
}

} // namespace ncs::server::middleware
