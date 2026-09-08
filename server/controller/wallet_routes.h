// 用户钱包路由（/user/wallet、/user/wallet/recharges、/user/wallet/transactions）：余额查询、充值与流水分页。
// 金额一律整数分；充值写操作经 BoundedExecutor 异步执行并强制 Idempotency-Key 幂等契约。
#pragma once

#include "core/application/bounded_executor.h"
#include "core/application/idempotency_service.h"
#include "core/application/session_manager.h"
#include "core/application/wallet_service.h"
#include "server/controller/api_routes.h"

namespace ncs::server::controller
{

class WalletRoutes final
{
  public:
    WalletRoutes(ApiRoutes& routes, core::application::WalletService& wallet,
                 core::application::SessionManager& sessions,
                 core::application::BoundedExecutor& executor,
                 core::application::IdempotencyService& idempotency);
};

} // namespace ncs::server::controller
