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

namespace ncs::core::application {

struct AdminStationPage {
  std::vector<Station> items;
  int total = 0;
  int page = 1;
  int pageSize = 20;
};

struct AdminChargerPage {
  std::vector<Charger> items;
  int total = 0;
  int page = 1;
  int pageSize = 20;
};

struct RestartCommandView {
  std::string commandNo;
  std::string status;
  int chargerStatus = 0;
  std::int64_t createdAt = 0;
};

struct StationPatch {
  std::optional<std::string> name;
  std::optional<std::string> address;
  std::optional<std::string> adcode;
  std::optional<std::int64_t> latitudeE6;
  std::optional<std::int64_t> longitudeE6;
  std::optional<std::string> businessHours;
};

// Section 6.2: station lifecycle, charger management, tariffs and approved
// service-fee adjustments. Reauthentication is enforced by the controller.
class AdminStationService final {
public:
  AdminStationService(AdminRepository &repository,
                      ChargingRepository &charging,
                      ChargeFlowService &flows, BusinessNumbers &numbers)
      : repository_(repository), charging_(charging), flows_(flows),
        numbers_(numbers) {}

  AdminStationPage stations(std::optional<int> status,
                            std::optional<std::string> adcode,
                            const std::string &keyword, int page,
                            int pageSize);
  ServiceResult<Station>
  createStation(std::int64_t actorAdminId, const Station &draft,
                const InitialChargerSpec &initialCharger,
                std::chrono::system_clock::time_point now);
  ServiceResult<Station>
  updateStation(std::int64_t actorAdminId, std::int64_t stationId,
                const StationPatch &patch, std::int64_t expectedVersion,
                std::chrono::system_clock::time_point now);
  ServiceResult<Station>
  setStationEnabled(std::int64_t actorAdminId, std::int64_t stationId,
                    bool enabled, const std::string &reason,
                    std::int64_t expectedVersion,
                    std::chrono::system_clock::time_point now);

  AdminChargerPage chargers(std::optional<std::int64_t> stationId,
                            std::optional<int> status,
                            std::optional<int> chargerType,
                            const std::string &keyword, int page,
                            int pageSize);
  ServiceResult<std::vector<Charger>>
  createChargers(std::int64_t actorAdminId, std::int64_t stationId,
                 const std::vector<Charger> &drafts,
                 std::chrono::system_clock::time_point now);
  ServiceResult<Charger>
  setChargerStatus(std::int64_t actorAdminId, std::int64_t chargerId,
                   int targetStatus, const std::string &reason,
                   std::int64_t expectedVersion,
                   std::chrono::system_clock::time_point now);
  ServiceResult<RestartCommandView> createRestartCommand(
      std::int64_t actorAdminId, std::int64_t chargerId,
      const std::string &reason, std::chrono::system_clock::time_point now);
  ServiceResult<DeviceCommand> deviceCommand(const std::string &commandNo,
                                             std::chrono::system_clock::time_point now);

  std::vector<RegionTariff> tariffs(std::optional<std::string> adcode);
  ServiceResult<RegionTariff>
  createTariff(std::int64_t actorAdminId, const RegionTariff &draft,
               const std::string &reason,
               std::chrono::system_clock::time_point now);
  ServiceResult<PriceAdjustment> createPriceAdjustment(
      std::int64_t actorAdminId, const PriceAdjustment &draft,
      const std::string &reason, std::chrono::system_clock::time_point now);

  // Runtime tick entry: finish simulated restart commands.
  void completeDueCommands(std::chrono::system_clock::time_point now);

  static bool validStationCode(std::string_view code);

private:
  std::int64_t effectiveAdjustmentBp(std::int64_t stationId, int chargerType,
                                     std::int64_t at);

  AdminRepository &repository_;
  ChargingRepository &charging_;
  ChargeFlowService &flows_;
  BusinessNumbers &numbers_;
};

} // namespace ncs::core::application
