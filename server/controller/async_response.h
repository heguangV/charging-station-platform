// 慢操作异步派发辅助：把阻塞闭包交给 BoundedExecutor，受每路径 deadline
// 约束，超时返回统一超时错误，队列满返回 429 + Retry-After。 供无需幂等键的接口使用，确保 Crow
// 事件循环不被 SQLite 或外部调用阻塞。
#pragma once

#include "core/application/bounded_executor.h"

#include <crow.h>

#include <functional>

namespace ncs::server::controller
{

void dispatchBlocking(const crow::request& request, crow::response& response,
                      core::application::BoundedExecutor& executor,
                      std::function<crow::response()> operation);

} // namespace ncs::server::controller
