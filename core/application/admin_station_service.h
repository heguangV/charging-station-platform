// 站点管理应用服务（实施指南
// §6.2）：站点生命周期、充电桩管理、区域资费版本、审定服务费调价与模拟重启命令。 依赖
// AdminRepository、ChargingRepository、ChargeFlowService（占用冲突校验）与
// BusinessNumbers（业务编号）；再认证由控制器层强制执行。 约束：站点/充电桩/资费写入经
// ChargingRepository 保持单一数据源；状态变更须携带 expectedVersion 乐观锁并记录审计原因。

#pragma once

#include "core/application/admin_repository.h"
#include "core/application/business_numbers.h"
#include "core/application/charge_flow_service.h"
#include "core/application/charging_repository.h"
#include "core/application/service_result.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ncs::core::application
{

struct AdminStationPage
{
    std::vector<Station> items;
    int total = 0;
    int page = 1;
    int pageSize = 20;
};

struct AdminChargerPage
{
    std::vector<Charger> items;
    int total = 0;
    int page = 1;
    int pageSize = 20;
};

struct RestartCommandView
{
    std::string commandNo;
    std::string status;
    int chargerStatus = 0;
    std::int64_t createdAt = 0;
};

struct StationPatch
{
    std::optional<std::string> name;
    std::optional<std::string> address;
    std::optional<std::string> adcode;
    std::optional<std::int64_t> latitudeE6;
    std::optional<std::int64_t> longitudeE6;
    std::optional<std::string> businessHours;
};

// Section 6.2: station lifecycle, charger management, tariffs and approved
// service-fee adjustments. Reauthentication is enforced by the controller.
class AdminStationService final
{
  public:
    AdminStationService(AdminRepository& repository, ChargingRepository& charging,
                        ChargeFlowService& flows, BusinessNumbers& numbers)
        : repository_(repository), charging_(charging), flows_(flows), numbers_(numbers)
    {
    }

    AdminStationPage stations(std::optional<int> status, std::optional<std::string> adcode,
                              const std::string& keyword, int page, int pageSize);
    // 创建站点并批量初始化充电桩（1~100 根，事务内）：站点编码唯一（重复返回
    // AlreadyExists），区域须已有生效价目表否则 ValidationFailed。
    ServiceResult<Station> createStation(std::int64_t actorAdminId, const Station& draft,
                                         const InitialChargerSpec& initialCharger,
                                         std::chrono::system_clock::time_point now);
    // 局部更新站点信息：expectedVersion 乐观锁（不符返回 VersionConflict）；改 adcode
    // 时须有生效价目表，整体校验失败返回 ValidationFailed。
    ServiceResult<Station> updateStation(std::int64_t actorAdminId, std::int64_t stationId,
                                         const StationPatch& patch, std::int64_t expectedVersion,
                                         std::chrono::system_clock::time_point now);
    // 启用/停用站点：停用时站点下不得存在进行中流程（InvalidStateTransition）；启用后立即触发两类桩型的队列晋级。
    ServiceResult<Station> setStationEnabled(std::int64_t actorAdminId, std::int64_t stationId,
                                             bool enabled, const std::string& reason,
                                             std::int64_t expectedVersion,
                                             std::chrono::system_clock::time_point now);

    AdminChargerPage chargers(std::optional<std::int64_t> stationId, std::optional<int> status,
                              std::optional<int> chargerType, const std::string& keyword, int page,
                              int pageSize);
    // 批量新增充电桩（≤100，事务内）：编码重复返回
    // AlreadyExists，任一桩字段非法则整体失败；成功后触发队列晋级。
    ServiceResult<std::vector<Charger>> createChargers(std::int64_t actorAdminId,
                                                       std::int64_t stationId,
                                                       const std::vector<Charger>& drafts,
                                                       std::chrono::system_clock::time_point now);
    // 变更充电桩状态（仅 0 空闲/2 故障/3 停用）：expectedVersion
    // 乐观锁（VersionConflict）；桩上存在进行中流程时拒绝（InvalidStateTransition，须走强制释放/重启），置回空闲时触发队列晋级。
    ServiceResult<Charger> setChargerStatus(std::int64_t actorAdminId, std::int64_t chargerId,
                                            int targetStatus, const std::string& reason,
                                            std::int64_t expectedVersion,
                                            std::chrono::system_clock::time_point now);
    // 创建模拟重启命令（UC-A-05）：先强制释放预约类流程或幂等结算充电中流程，设备置 1 重启中，命令
    // 2 秒后到期执行；流程处置失败则整体回滚。
    ServiceResult<RestartCommandView>
    createRestartCommand(std::int64_t actorAdminId, std::int64_t chargerId,
                         const std::string& reason, std::chrono::system_clock::time_point now);
    ServiceResult<DeviceCommand> deviceCommand(const std::string& commandNo,
                                               std::chrono::system_clock::time_point now);

    std::vector<RegionTariff> tariffs(std::optional<std::string> adcode);
    // 新增区域价目版本：与既有版本时间区间重叠时返回 ValidationFailed；effectiveTo 未给则置 2100
    // 年远期。
    ServiceResult<RegionTariff> createTariff(std::int64_t actorAdminId, const RegionTariff& draft,
                                             const std::string& reason,
                                             std::chrono::system_clock::time_point now);
    // 审定站点服务费调价：幅度限 ±2000bp 且为 500 的整数倍，来源仅
    // ML_APPROVED/MANUAL，站点须存在，否则 ValidationFailed。
    ServiceResult<PriceAdjustment> createPriceAdjustment(std::int64_t actorAdminId,
                                                         const PriceAdjustment& draft,
                                                         const std::string& reason,
                                                         std::chrono::system_clock::time_point now);

    // Runtime tick entry: finish simulated restart commands.
    void completeDueCommands(std::chrono::system_clock::time_point now);

    static bool validStationCode(std::string_view code);

  private:
    std::int64_t effectiveAdjustmentBp(std::int64_t stationId, int chargerType, std::int64_t at);

    AdminRepository& repository_;
    ChargingRepository& charging_;
    ChargeFlowService& flows_;
    BusinessNumbers& numbers_;
};

} // namespace ncs::core::application
