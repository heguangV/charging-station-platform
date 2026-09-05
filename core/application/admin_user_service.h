#pragma once

#include "core/application/admin_repository.h"
#include "core/application/charge_flow_service.h"
#include "core/application/service_result.h"

namespace ncs::core::application {

struct AdminUserDetail {
  UserAccount user;
  std::size_t activeSessionCount = 0;
  ActiveFlowView activeFlow;
};

struct AdminStatusUpdate {
  UserAccount user;
  bool activeFlowPreserved = false;
};

class AdminUserService final {
public:
  AdminUserService(AdminRepository &repository, ChargeFlowService &flows,
                   SessionManager &sessions)
      : repository_(repository), flows_(flows), sessions_(sessions) {}

  AdminUserPage list(const AdminUserQuery &query);
  ServiceResult<AdminUserDetail>
  detail(std::int64_t userId, std::chrono::system_clock::time_point now);
  ServiceResult<AdminStatusUpdate>
  updateStatus(std::int64_t actorAdminId, std::int64_t userId, int status,
               std::string reason, std::int64_t expectedVersion,
               std::chrono::system_clock::time_point now);
  ServiceResult<OrderPage>
  orders(std::int64_t actorAdminId, std::int64_t userId,
         std::optional<int> status, std::int64_t fromAt, std::int64_t toAt,
         const std::string &sort, int page, int pageSize,
         std::chrono::system_clock::time_point now);

private:
  static bool validReason(std::string_view reason);
  static std::string userPrincipal(std::int64_t userId);

  AdminRepository &repository_;
  ChargeFlowService &flows_;
  SessionManager &sessions_;
};

} // namespace ncs::core::application
