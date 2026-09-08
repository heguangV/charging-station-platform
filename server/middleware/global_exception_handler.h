// 全局异常兜底：挂到 ServerApp 的 exception_handler，任何未处理异常清空响应并返回 500 +
// INTERNAL_ERROR 固定信封。 绝不向客户端泄露 SQL/堆栈/路径等内部信息，仅记录受保护日志。
#pragma once

#include "server/server_app.h"

namespace ncs::infrastructure::files
{
class StructuredLogger;
}

namespace ncs::server::middleware
{

void installGlobalExceptionHandler(ServerApp& application,
                                   infrastructure::files::StructuredLogger& logger);

} // namespace ncs::server::middleware
