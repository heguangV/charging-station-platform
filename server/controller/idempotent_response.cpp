#include "server/controller/idempotent_response.h"

#include "core/domain/error_code.h"
#include "server/controller/api_response.h"
#include "server/controller/async_response.h"
#include "server/controller/request_validation.h"

#include <utility>

namespace ncs::server::controller {

void dispatchIdempotentBlocking(
    const crow::request &request, crow::response &response,
    core::application::BoundedExecutor &executor,
    core::application::IdempotencyService &idempotency,
    const std::string_view scope, const bool permanent,
    std::function<crow::response(core::application::IdempotencyLease &)>
        operation) {
  const auto key = idempotencyKey(request);
  if (!key) {
    response = errorResponse(core::domain::ErrorCode::InvalidArgument,
                             "idempotency key is missing or invalid",
                             "缺少有效的幂等键");
    response.end();
    return;
  }
  dispatchBlocking(
      request, response, executor,
      [&idempotency, scope = std::string(scope), key = std::string(*key),
       requestBody = request.body, permanent,
       operation = std::move(operation)]() mutable {
        const auto now = std::chrono::system_clock::now();
        const auto check =
            idempotency.begin(scope, key, requestBody, now, permanent);
        switch (check.decision) {
        case core::application::IdempotencyDecision::Replay: {
          const auto &stored = *check.replay;
          crow::response replay(stored.status);
          replay.body = stored.body;
          replay.set_header("Content-Type", stored.contentType);
          return replay;
        }
        case core::application::IdempotencyDecision::InProgress:
          return errorResponse(core::domain::ErrorCode::IdempotencyConflict,
                               "an identical request is still in progress",
                               "相同请求正在处理，请稍后重试");
        case core::application::IdempotencyDecision::Conflict:
          return errorResponse(
              core::domain::ErrorCode::IdempotencyConflict,
              "idempotency key was used for a different request",
              "幂等键已被用于不同的请求");
        case core::application::IdempotencyDecision::CapacityExceeded: {
          auto unavailable = errorResponse(
              core::domain::ErrorCode::TransactionFailed,
              "too many pending idempotent operations", "服务繁忙，请稍后重试");
          unavailable.set_header("Retry-After", "1");
          return unavailable;
        }
        case core::application::IdempotencyDecision::InvalidKey:
          return errorResponse(core::domain::ErrorCode::InvalidArgument,
                               "idempotency key is invalid",
                               "幂等键不符合要求");
        case core::application::IdempotencyDecision::Proceed:
          break;
        }

        core::application::IdempotencyLease lease(idempotency, scope, key,
                                                  *check.leaseToken);
        crow::response result;
        core::application::StoredHttpResult stored;
        const bool completed = lease.executeAndComplete(
            [&] {
              result = operation(lease);
              return core::application::StoredHttpResult{
                  result.code,
                  result.get_header_value("Content-Type").empty()
                      ? "application/json; charset=utf-8"
                      : result.get_header_value("Content-Type"),
                  result.body,
              };
            },
            stored, std::chrono::system_clock::now());
        if (!completed) {
          return errorResponse(core::domain::ErrorCode::IdempotencyConflict,
                               "idempotency lease is no longer valid",
                               "请求状态已变化，请重新查询结果");
        }
        return result;
      });
}

} // namespace ncs::server::controller
