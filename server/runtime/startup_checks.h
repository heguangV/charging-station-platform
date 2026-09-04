#pragma once

#include "server/runtime/server_config.h"

#include <asio/ssl/context.hpp>

namespace ncs::server::runtime {

void runStartupChecks(const ServerConfig &config);
asio::ssl::context createTlsContext(const ServerConfig &config);

} // namespace ncs::server::runtime
