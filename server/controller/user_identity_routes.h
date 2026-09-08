// 用户身份路由（/user/auth/* 注册登录短信、/user/sessions 会话管理、/user/me 资料与头像）。
// 慢操作经 BoundedExecutor
// 异步执行；开发模式（developmentMode）下允许模拟短信验证码，正式环境必须关闭。
#pragma once

#include "core/application/bounded_executor.h"
#include "core/application/session_manager.h"
#include "core/application/user_identity_service.h"
#include "server/controller/api_routes.h"

namespace ncs::server::controller
{

class UserIdentityRoutes final
{
  public:
    UserIdentityRoutes(ApiRoutes& routes, core::application::UserIdentityService& identity,
                       core::application::SessionManager& sessions,
                       core::application::BoundedExecutor& blockingExecutor, bool developmentMode);
};

} // namespace ncs::server::controller
