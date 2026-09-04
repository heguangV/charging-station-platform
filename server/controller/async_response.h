#pragma once

#include "core/application/bounded_executor.h"

#include <crow.h>

#include <functional>

namespace ncs::server::controller {

void dispatchBlocking(
    const crow::request &request,
    crow::response &response,
    core::application::BoundedExecutor &executor,
    std::function<crow::response()> operation);

} // namespace ncs::server::controller
