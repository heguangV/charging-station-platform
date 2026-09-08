// REST 路由注册基座：统一持有 /api/v1 前缀，向 Crow ServerApp 申请 DynamicRule 供各业务路由类挂载。
// 所有 controller 路由（user/admin/system/internal）都经由它创建，保证版本前缀一致。
#pragma once

#include "server/server_app.h"

#include <string>
#include <string_view>

namespace ncs::server::controller
{

class ApiRoutes final
{
  public:
    static constexpr std::string_view routePrefix = "/api/v1";

    explicit ApiRoutes(ServerApp& application);

    crow::DynamicRule& route(std::string_view relativePath);

  private:
    ServerApp& application_;
};

} // namespace ncs::server::controller
