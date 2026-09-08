// 管理端用户管理应用服务：用户分页列表、详情（含活跃会话与进行中充电）、冻结/解冻与用户订单查询。
// 依赖 AdminRepository（查询与审计）、ChargeFlowService（订单与活跃流程）与
// SessionManager（会话吊销）。 约束：状态变更须携带变更原因与
// expectedVersion；冻结（status=0）即吊销该用户全部会话，冻结时有进行中流程以 activeFlowPreserved
// 标记。

#pragma once

#include "core/application/admin_repository.h"
#include "core/application/charge_flow_service.h"
#include "core/application/service_result.h"

namespace ncs::core::application
{

struct AdminUserDetail
{
    UserAccount user;
    std::size_t activeSessionCount = 0;
    ActiveFlowView activeFlow;
};

struct AdminStatusUpdate
{
    UserAccount user;
    bool activeFlowPreserved = false;
};

class AdminUserService final
{
  public:
    AdminUserService(AdminRepository& repository, ChargeFlowService& flows,
                     SessionManager& sessions)
        : repository_(repository), flows_(flows), sessions_(sessions)
    {
    }

    AdminUserPage list(const AdminUserQuery& query);
    // 用户详情：账号资料 + 活跃会话数 + 进行中充电流程；用户不存在返回 NotFound。
    ServiceResult<AdminUserDetail> detail(std::int64_t userId,
                                          std::chrono::system_clock::time_point now);
    // 冻结/解冻用户：expectedVersion
    // 乐观锁（VersionConflict）；冻结（status=0）即吊销该用户全部会话，冻结时若有进行中流程以
    // activeFlowPreserved 标记。
    ServiceResult<AdminStatusUpdate> updateStatus(std::int64_t actorAdminId, std::int64_t userId,
                                                  int status, std::string reason,
                                                  std::int64_t expectedVersion,
                                                  std::chrono::system_clock::time_point now);
    // 查询指定用户的订单分页（委托 ChargeFlowService）：用户不存在返回
    // NotFound；查询成功在同一事务内记录 USER_ORDERS_VIEWED 审计。
    ServiceResult<OrderPage> orders(std::int64_t actorAdminId, std::int64_t userId,
                                    std::optional<int> status, std::int64_t fromAt,
                                    std::int64_t toAt, const std::string& sort, int page,
                                    int pageSize, std::chrono::system_clock::time_point now);

  private:
    static bool validReason(std::string_view reason);
    static std::string userPrincipal(std::int64_t userId);

    AdminRepository& repository_;
    ChargeFlowService& flows_;
    SessionManager& sessions_;
};

} // namespace ncs::core::application
