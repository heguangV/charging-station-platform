#pragma once

#include "core/application/bounded_executor.h"
#include "core/application/idempotency_service.h"

#include <crow.h>

#include <functional>
#include <string_view>

namespace ncs::server::controller {

// Runs a blocking write command under the Idempotency-Key contract: missing or
// malformed keys reject synchronously, replays return the stored HTTP result,
// and the operation's final response is stored for later retries. The lease is
// completed inside the worker; any exception path leaves it to the RAII guard,
// which aborts the reservation so a retry can start over.
void dispatchIdempotentBlocking(
    const crow::request &request,
    crow::response &response,
    core::application::BoundedExecutor &executor,
    core::application::IdempotencyService &idempotency,
    std::string_view scope,
    bool permanent,
    std::function<crow::response(core::application::IdempotencyLease &)> operation);

} // namespace ncs::server::controller
