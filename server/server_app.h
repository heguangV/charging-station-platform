#pragma once

#include "server/middleware/request_policy_middleware.h"

#include <crow.h>

namespace ncs::server {

using ServerApp = crow::App<middleware::RequestPolicyMiddleware>;

} // namespace ncs::server
