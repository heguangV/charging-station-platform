#include "server/controller/async_response.h"

#include "core/domain/error_code.h"
#include "server/controller/api_response.h"
#include "server/middleware/request_policy_middleware.h"

#include <asio/post.hpp>
#include <asio/steady_timer.hpp>

#include <atomic>
#include <memory>
#include <utility>

namespace ncs::server::controller
{
namespace
{

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

// 包装业务操作：任何异常都不得逃逸出 worker 线程，统一降级为 InternalError 错误响应。
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

// 阻塞任务派发：把 operation 提交到有界阻塞线程池，与按路径确定的超时定时器竞速，
// claimed 原子标志保证超时响应与正常响应只有一方送达（超时映射 ExternalServiceUnavailable）；
// 无 io_context 时同步执行兜底，队列已满时以 RateLimited（附 Retry-After:1）短路。
void dispatchBlocking(const crow::request& request, crow::response& response,
                      core::application::BoundedExecutor& executor,
                      std::function<crow::response()> operation)
{
    if (!request.io_context)
    {
        response = safeOperation(operation);
        response.end();
        return;
    }

    auto state = std::make_shared<DispatchState>(*request.io_context);
    asio::io_context* ioContext = request.io_context;
    state->timer.expires_after(middleware::RequestPolicyMiddleware::deadlineForPath(request.url));
    crow::response* responsePointer = &response;
    state->timer.async_wait(
        [state, responsePointer](const asio::error_code& error)
        {
            if (error || state->claimed.exchange(true))
                return;
            *responsePointer = timeoutResponse();
            responsePointer->end();
        });

    const bool accepted = executor.submit(
        [state, responsePointer, ioContext, operation = std::move(operation)]() mutable
        {
            crow::response result = safeOperation(operation);
            asio::post(*ioContext,
                       [state, responsePointer, result = std::move(result)]() mutable
                       {
                           if (state->claimed.exchange(true))
                               return;
                           state->timer.cancel();
                           *responsePointer = std::move(result);
                           responsePointer->end();
                       });
        });
    if (!accepted && !state->claimed.exchange(true))
    {
        state->timer.cancel();
        response = unavailableResponse();
        response.end();
    }
}

} // namespace ncs::server::controller
