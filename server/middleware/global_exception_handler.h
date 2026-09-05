#pragma once

#include "server/server_app.h"

namespace ncs::infrastructure::files {
class StructuredLogger;
}

namespace ncs::server::middleware {

void installGlobalExceptionHandler(
    ServerApp &application,
    infrastructure::files::StructuredLogger &logger);

} // namespace ncs::server::middleware
