#include "server/controller/async_response.h"

#include "core/domain/error_code.h"
#include "server/controller/api_response.h"
#include "server/middleware/request_policy_middleware.h"

#include <asio/post.hpp>
#include <asio/steady_timer.hpp>

#include <atomic>
#include <cstdio>
#include <memory>
#include <utility>

namespace ncs::server::controller
{
namespace
{

// Temporary CI diagnostics for the Windows HTTPS smoke-test hang.
void asyncTrace(const char* stage)
{
    std::fprintf(stderr, "[async-debug] %s\n", stage);
    std::fflush(stderr);
}

struct DispatchState
{
    explicit DispatchState(asio::io_context& context) : timer(context) {}

    std::atomic_bool claimed{false};
    asio::steady_timer timer;
};

crow::response unavailableResponse()
{
    auto response = errorResponse(core::domain::ErrorCode::RateLimited,
                                  "blocking work queue is full", "服务繁忙，请稍后重试");
    response.set_header("Retry-After", "1");
    return response;
}

crow::response timeoutResponse()
{
    return errorResponse(core::domain::ErrorCode::ExternalServiceUnavailable,
                         "request deadline exceeded", "请求处理超时，请重试");
}

crow::response safeOperation(std::function<crow::response()>& operation)
{
    try
    {
        return operation();
    }
    catch (...)
    {
        return errorResponse(core::domain::ErrorCode::InternalError, "internal error",
                             "服务暂时不可用，请稍后重试");
    }
}

} // namespace

void dispatchBlocking(const crow::request& request, crow::response& response,
                      core::application::BoundedExecutor& executor,
                      std::function<crow::response()> operation)
{
    asyncTrace("dispatchBlocking entered");
    try
    {
        if (!request.io_context)
        {
            asyncTrace("no io_context, completing synchronously");
            response = safeOperation(operation);
            response.end();
            return;
        }

        auto state = std::make_shared<DispatchState>(*request.io_context);
        asio::io_context* ioContext = request.io_context;
        state->timer.expires_after(
            middleware::RequestPolicyMiddleware::deadlineForPath(request.url));
        crow::response* responsePointer = &response;
        state->timer.async_wait(
            [state, responsePointer](const asio::error_code& error)
            {
                try
                {
                    asyncTrace("timer handler entered");
                    if (error || state->claimed.exchange(true))
                    {
                        asyncTrace(error ? "timer fired with error (cancelled)"
                                         : "timer fired but claimed");
                        return;
                    }
                    asyncTrace("timer claiming and writing timeout response");
                    *responsePointer = timeoutResponse();
                    responsePointer->end();
                    asyncTrace("timer end() returned");
                }
                catch (const std::exception& exception)
                {
                    std::fprintf(stderr, "[async-debug] timer threw: %s\n", exception.what());
                    std::fflush(stderr);
                    throw;
                }
            });

        asyncTrace("arming timer and submitting work");
        const bool accepted = executor.submit(
            [state, responsePointer, ioContext, operation = std::move(operation)]() mutable
            {
                try
                {
                    asyncTrace("worker running operation");
                    crow::response result = safeOperation(operation);
                    asyncTrace("worker operation finished, posting completion");
                    asio::post(*ioContext,
                               [state, responsePointer, result = std::move(result)]() mutable
                               {
                                   try
                                   {
                                       asyncTrace("posted completion handler entered");
                                       if (state->claimed.exchange(true))
                                       {
                                           asyncTrace("posted completion lost the claim");
                                           return;
                                       }
                                       state->timer.cancel();
                                       *responsePointer = std::move(result);
                                       responsePointer->end();
                                       asyncTrace("posted end() returned");
                                   }
                                   catch (const std::exception& exception)
                                   {
                                       std::fprintf(stderr,
                                                    "[async-debug] posted handler threw: %s\n",
                                                    exception.what());
                                       std::fflush(stderr);
                                       throw;
                                   }
                               });
                }
                catch (const std::exception& exception)
                {
                    std::fprintf(stderr, "[async-debug] worker task threw: %s\n", exception.what());
                    std::fflush(stderr);
                    throw;
                }
            });
        asyncTrace(accepted ? "work submitted" : "work rejected (queue full)");
        if (!accepted && !state->claimed.exchange(true))
        {
            state->timer.cancel();
            response = unavailableResponse();
            response.end();
        }
    }
    catch (const std::exception& exception)
    {
        asyncTrace("dispatchBlocking threw");
        std::fprintf(stderr, "[async-debug] exception: %s\n", exception.what());
        std::fflush(stderr);
        throw;
    }
    catch (...)
    {
        asyncTrace("dispatchBlocking threw non-std exception");
        throw;
    }
}

} // namespace ncs::server::controller
