#include "core/application/admin_station_service.h"

#include <algorithm>
#include <charconv>

namespace ncs::core::application {
namespace {

constexpr std::int64_t kFarFutureSeconds = 4102444800; // 2100-01-01 UTC

std::int64_t unixSeconds(const std::chrono::system_clock::time_point value) {
  return std::chrono::duration_cast<std::chrono::seconds>(
             value.time_since_epoch())
      .count();
}

bool validReason(const std::string_view reason) {
  if (reason.size() < 2 || reason.size() > 200)
    return false;
  for (const unsigned char character : reason) {
    if (character < 0x20 || character == 0x7f)
      return false;
  }
  return true;
}

bool validCoordinates(const std::int64_t latitudeE6,
                      const std::int64_t longitudeE6) {
  return latitudeE6 >= -90000000 && latitudeE6 <= 90000000 &&
         longitudeE6 >= -180000000 && longitudeE6 <= 180000000;
}

bool validAdcode(const std::string_view adcode) {
  return adcode.size() == 6 &&
         adcode.find_first_not_of("0123456789") == std::string_view::npos;
}

} // namespace

bool AdminStationService::validStationCode(const std::string_view code) {
  return code.size() >= 2 && code.size() <= 16 &&
         code.find_first_not_of(
             "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") ==
             std::string_view::npos;
}

AdminStationPage AdminStationService::stations(
    const std::optional<int> status, const std::optional<std::string> adcode,
    const std::string &keyword, const int page, const int pageSize) {
  auto all = repository_.stations(status, adcode, keyword);
  std::sort(all.begin(), all.end(),
            [](const Station &left, const Station &right) {
              return left.id < right.id;
            });
  AdminStationPage result;
  result.total = static_cast<int>(all.size());
  result.page = page;
  result.pageSize = pageSize;
  const std::size_t first = static_cast<std::size_t>(page - 1) * pageSize;
  for (std::size_t index = first;
       index < all.size() &&
       result.items.size() < static_cast<std::size_t>(pageSize);
       ++index) {
    result.items.push_back(all[index]);
  }
  return result;
}

ServiceResult<Station> AdminStationService::createStation(
    const std::int64_t actorAdminId, const Station &draft,
    const InitialChargerSpec &initialCharger,
    const std::chrono::system_clock::time_point now) {
  const auto nowSeconds = unixSeconds(now);
  if (!validStationCode(draft.code) || draft.name.empty() ||
      draft.name.size() > 64 || draft.address.empty() ||
      draft.address.size() > 128 || draft.adcode.size() != 6 ||
      !validAdcode(draft.adcode) ||
      !validCoordinates(draft.latitudeE6, draft.longitudeE6) ||
      initialCharger.count < 1 || initialCharger.count > 100 ||
      initialCharger.powerWatt < 1 ||
      initialCharger.powerWatt > 1000000) {
    return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
  }
  const auto tariff = charging_.effectiveTariff(draft.adcode, nowSeconds);
  if (!tariff)
    return {core::domain::ErrorCode::ValidationFailed, std::nullopt};

  Station station = draft;
  station.enabled = true;
  station.version = 1;
  core::domain::ErrorCode error = core::domain::ErrorCode::Ok;
  repository_.withTransaction([&] {
    if (repository_.stationCodeExists(station.code)) {
      error = core::domain::ErrorCode::AlreadyExists;
      return;
    }
    if (!repository_.createStationWithChargers(station, initialCharger)) {
      error = core::domain::ErrorCode::AlreadyExists;
      return;
    }
    repository_.addAuditEvent(AuditEvent{
        actorAdminId, "STATION_CREATED", "STATION", std::to_string(station.id),
        station.code, nowSeconds});
  });
  if (error != core::domain::ErrorCode::Ok)
    return {error, std::nullopt};
  return {core::domain::ErrorCode::Ok, station};
}

ServiceResult<Station> AdminStationService::updateStation(
    const std::int64_t actorAdminId, const std::int64_t stationId,
    const StationPatch &patch, const std::int64_t expectedVersion,
    const std::chrono::system_clock::time_point now) {
  const auto nowSeconds = unixSeconds(now);
  core::domain::ErrorCode error = core::domain::ErrorCode::Ok;
  std::optional<Station> updated;
  repository_.withTransaction([&] {
    const auto current = charging_.station(stationId);
    if (!current) {
      error = core::domain::ErrorCode::NotFound;
      return;
    }
    if (current->version != expectedVersion) {
      error = core::domain::ErrorCode::VersionConflict;
      return;
    }
    Station next = *current;
    if (patch.name)
      next.name = *patch.name;
    if (patch.address)
      next.address = *patch.address;
    if (patch.businessHours)
      next.businessHours = *patch.businessHours;
    if (patch.latitudeE6)
      next.latitudeE6 = *patch.latitudeE6;
    if (patch.longitudeE6)
      next.longitudeE6 = *patch.longitudeE6;
    if (patch.adcode && *patch.adcode != next.adcode) {
      if (!validAdcode(*patch.adcode) ||
          !charging_.effectiveTariff(*patch.adcode, nowSeconds)) {
        error = core::domain::ErrorCode::ValidationFailed;
        return;
      }
      next.adcode = *patch.adcode;
    }
    if (next.name.empty() || next.name.size() > 64 || next.address.empty() ||
        next.address.size() > 128 || !validAdcode(next.adcode) ||
        !validCoordinates(next.latitudeE6, next.longitudeE6) ||
        next.businessHours.size() > 64) {
      error = core::domain::ErrorCode::ValidationFailed;
      return;
    }
    ++next.version;
    if (!repository_.saveStation(next)) {
      error = core::domain::ErrorCode::NotFound;
      return;
    }
    repository_.addAuditEvent(AuditEvent{
        actorAdminId, "STATION_UPDATED", "STATION", std::to_string(stationId),
        {}, nowSeconds});
    updated = next;
  });
  if (error != core::domain::ErrorCode::Ok)
    return {error, std::nullopt};
  return {core::domain::ErrorCode::Ok, *updated};
}

ServiceResult<Station> AdminStationService::setStationEnabled(
    const std::int64_t actorAdminId, const std::int64_t stationId,
    const bool enabled, const std::string &reason,
    const std::int64_t expectedVersion,
    const std::chrono::system_clock::time_point now) {
  if (!validReason(reason))
    return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
  const auto nowSeconds = unixSeconds(now);
  core::domain::ErrorCode error = core::domain::ErrorCode::Ok;
  std::optional<Station> updated;
  repository_.withTransaction([&] {
    const auto current = charging_.station(stationId);
    if (!current) {
      error = core::domain::ErrorCode::NotFound;
      return;
    }
    if (current->version != expectedVersion) {
      error = core::domain::ErrorCode::VersionConflict;
      return;
    }
    if (!enabled && repository_.stationHasActiveFlow(stationId)) {
      error = core::domain::ErrorCode::InvalidStateTransition;
      return;
    }
    Station next = *current;
    next.enabled = enabled;
    ++next.version;
    if (!repository_.saveStation(next)) {
      error = core::domain::ErrorCode::NotFound;
      return;
    }
    repository_.addAuditEvent(AuditEvent{
        actorAdminId, enabled ? "STATION_ENABLED" : "STATION_DISABLED",
        "STATION", std::to_string(stationId), reason, nowSeconds});
    if (enabled) {
      flows_.promoteAvailable(stationId, ChargerType::AcSlow, now);
      flows_.promoteAvailable(stationId, ChargerType::DcFast, now);
    }
    updated = next;
  });
  if (error != core::domain::ErrorCode::Ok)
    return {error, std::nullopt};
  return {core::domain::ErrorCode::Ok, *updated};
}

AdminChargerPage AdminStationService::chargers(
    const std::optional<std::int64_t> stationId,
    const std::optional<int> status, const std::optional<int> chargerType,
    const std::string &keyword, const int page, const int pageSize) {
  std::optional<ChargerStatus> chargerStatus;
  if (status && *status >= 0 && *status <= 4)
    chargerStatus = static_cast<ChargerStatus>(*status);
  std::optional<ChargerType> type;
  if (chargerType && (*chargerType == 0 || *chargerType == 1))
    type = static_cast<ChargerType>(*chargerType);
  auto all = charging_.chargers(stationId, type, chargerStatus);
  std::vector<Charger> filtered;
  for (auto &charger : all) {
    if (!keyword.empty() && charger.code.find(keyword) == std::string::npos)
      continue;
    filtered.push_back(charger);
  }
  AdminChargerPage result;
  result.total = static_cast<int>(filtered.size());
  result.page = page;
  result.pageSize = pageSize;
  const std::size_t first = static_cast<std::size_t>(page - 1) * pageSize;
  for (std::size_t index = first;
       index < filtered.size() &&
       result.items.size() < static_cast<std::size_t>(pageSize);
       ++index) {
    result.items.push_back(filtered[index]);
  }
  return result;
}

ServiceResult<std::vector<Charger>> AdminStationService::createChargers(
    const std::int64_t actorAdminId, const std::int64_t stationId,
    const std::vector<Charger> &drafts,
    const std::chrono::system_clock::time_point now) {
  const auto nowSeconds = unixSeconds(now);
  if (drafts.empty() || drafts.size() > 100)
    return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
  core::domain::ErrorCode error = core::domain::ErrorCode::Ok;
  std::vector<Charger> created = drafts;
  repository_.withTransaction([&] {
    if (!charging_.station(stationId)) {
      error = core::domain::ErrorCode::NotFound;
      return;
    }
    for (auto &charger : created) {
      charger.stationId = stationId;
      charger.id = 0;
      charger.status = ChargerStatus::Idle;
      charger.version = 1;
      if (charger.code.empty() || charger.code.size() > 32 ||
          charger.powerWatt < 1 || charger.powerWatt > 1000000 ||
          (charger.type != ChargerType::AcSlow &&
           charger.type != ChargerType::DcFast)) {
        error = core::domain::ErrorCode::ValidationFailed;
        return;
      }
    }
    if (!repository_.addChargers(created)) {
      error = core::domain::ErrorCode::AlreadyExists;
      return;
    }
    repository_.addAuditEvent(AuditEvent{
        actorAdminId, "CHARGER_BATCH_CREATED", "STATION",
        std::to_string(stationId),
        std::to_string(created.size()) + " chargers", nowSeconds});
    flows_.promoteAvailable(stationId, ChargerType::AcSlow, now);
    flows_.promoteAvailable(stationId, ChargerType::DcFast, now);
  });
  if (error != core::domain::ErrorCode::Ok)
    return {error, std::nullopt};
  return {core::domain::ErrorCode::Ok, created};
}

ServiceResult<Charger> AdminStationService::setChargerStatus(
    const std::int64_t actorAdminId, const std::int64_t chargerId,
    const int targetStatus, const std::string &reason,
    const std::int64_t expectedVersion,
    const std::chrono::system_clock::time_point now) {
  if (!validReason(reason) || (targetStatus != 0 && targetStatus != 2 &&
                               targetStatus != 3)) {
    return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
  }
  const auto nowSeconds = unixSeconds(now);
  core::domain::ErrorCode error = core::domain::ErrorCode::Ok;
  std::optional<Charger> updated;
  repository_.withTransaction([&] {
    const auto charger = charging_.charger(chargerId);
    if (!charger) {
      error = core::domain::ErrorCode::NotFound;
      return;
    }
    if (charger->version != expectedVersion) {
      error = core::domain::ErrorCode::VersionConflict;
      return;
    }
    if (repository_.activeFlowOnCharger(chargerId)) {
      // An active reservation or session must be released through the
      // force-release or restart flows instead (BR-11).
      error = core::domain::ErrorCode::InvalidStateTransition;
      return;
    }
    Charger next = *charger;
    next.status = static_cast<ChargerStatus>(targetStatus);
    ++next.version;
    charging_.saveCharger(next);
    charging_.addChargerStatusEvent(
        ChargerStatusEvent{chargerId, charger->stationId,
                           static_cast<int>(charger->status), targetStatus,
                           reason, nowSeconds});
    repository_.addAuditEvent(AuditEvent{
        actorAdminId, "CHARGER_STATUS_CHANGED", "CHARGER",
        std::to_string(chargerId), reason, nowSeconds});
    if (next.status == ChargerStatus::Idle)
      flows_.promoteAvailable(next.stationId, next.type, now);
    updated = next;
  });
  if (error != core::domain::ErrorCode::Ok)
    return {error, std::nullopt};
  return {core::domain::ErrorCode::Ok, *updated};
}

ServiceResult<RestartCommandView> AdminStationService::createRestartCommand(
    const std::int64_t actorAdminId, const std::int64_t chargerId,
    const std::string &reason, const std::chrono::system_clock::time_point now) {
  if (!validReason(reason))
    return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
  const auto nowSeconds = unixSeconds(now);
  core::domain::ErrorCode error = core::domain::ErrorCode::Ok;
  std::optional<RestartCommandView> view;
  repository_.withTransaction([&] {
    const auto charger = charging_.charger(chargerId);
    if (!charger) {
      error = core::domain::ErrorCode::NotFound;
      return;
    }
    if (charger->status == ChargerStatus::Disabled ||
        charger->status == ChargerStatus::Restarting) {
      error = core::domain::ErrorCode::InvalidStateTransition;
      return;
    }
    // UC-A-05: pending-quote, reserved and settling devices are released
    // first; charging devices settle idempotently before the restart. Any
    // failure must not release the device.
    const auto activeFlow = repository_.activeFlowOnCharger(chargerId);
    if (activeFlow) {
      const int status = activeFlow->status;
      if (status == static_cast<int>(FlowStatus::PendingQuote) ||
          status == static_cast<int>(FlowStatus::Reserved)) {
        const auto released = flows_.adminForceRelease(
            activeFlow->flowNo, reason,
            static_cast<int>(ChargerStatus::Restarting), activeFlow->version,
            now);
        if (!released.ok()) {
          error = released.error;
          return;
        }
      } else {
        const auto settled = flows_.adminControlledSettle(
            activeFlow->flowNo, reason,
            static_cast<int>(ChargerStatus::Restarting), now);
        if (!settled.ok()) {
          error = settled.error;
          return;
        }
      }
    }
    auto restarting = charging_.charger(chargerId);
    if (!restarting) {
      error = core::domain::ErrorCode::NotFound;
      return;
    }
    if (restarting->status != ChargerStatus::Restarting) {
      const int fromStatus = static_cast<int>(restarting->status);
      restarting->status = ChargerStatus::Restarting;
      ++restarting->version;
      charging_.saveCharger(*restarting);
      charging_.addChargerStatusEvent(
          ChargerStatusEvent{chargerId, restarting->stationId, fromStatus,
                             static_cast<int>(ChargerStatus::Restarting),
                             reason, nowSeconds});
    }

    DeviceCommand command;
    command.commandNo = numbers_.next("CMD", now);
    command.chargerId = chargerId;
    command.chargerCode = charger->code;
    command.status = "PENDING";
    command.reason = reason;
    command.actorId = std::to_string(actorAdminId);
    command.createdAt = nowSeconds;
    command.executeAt = nowSeconds + 2;
    repository_.addDeviceCommand(command);
    repository_.addAuditEvent(AuditEvent{
        actorAdminId, "RESTART_COMMAND", "CHARGER", std::to_string(chargerId),
        reason, nowSeconds});
    view = RestartCommandView{command.commandNo, command.status,
                              static_cast<int>(ChargerStatus::Restarting),
                              command.createdAt};
  });
  if (error != core::domain::ErrorCode::Ok)
    return {error, std::nullopt};
  return {core::domain::ErrorCode::Ok, *view};
}

ServiceResult<DeviceCommand> AdminStationService::deviceCommand(
    const std::string &commandNo,
    const std::chrono::system_clock::time_point now) {
  const auto command = repository_.deviceCommand(commandNo);
  if (!command)
    return {core::domain::ErrorCode::NotFound, std::nullopt};
  if (command->status == "PENDING" &&
      command->executeAt <= unixSeconds(now)) {
    completeDueCommands(now);
    const auto completed = repository_.deviceCommand(commandNo);
    if (!completed)
      return {core::domain::ErrorCode::NotFound, std::nullopt};
    return {core::domain::ErrorCode::Ok, *completed};
  }
  return {core::domain::ErrorCode::Ok, *command};
}

std::vector<RegionTariff> AdminStationService::tariffs(
    std::optional<std::string> adcode) {
  return repository_.tariffVersions(std::move(adcode));
}

ServiceResult<RegionTariff> AdminStationService::createTariff(
    const std::int64_t actorAdminId, const RegionTariff &draft,
    const std::string &reason, const std::chrono::system_clock::time_point now) {
  if (!validReason(reason) || draft.adcode.size() != 6 ||
      draft.adcode.find_first_not_of("0123456789") != std::string::npos ||
      draft.electricityCentPerKwh < 0 || draft.serviceCentPerKwh < 0 ||
      draft.electricityCentPerKwh > 100000 ||
      draft.serviceCentPerKwh > 100000 || draft.effectiveFrom < 0) {
    return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
  }
  RegionTariff tariff = draft;
  tariff.effectiveTo =
      draft.effectiveTo > draft.effectiveFrom
          ? draft.effectiveTo
          : kFarFutureSeconds;
  core::domain::ErrorCode error = core::domain::ErrorCode::Ok;
  repository_.withTransaction([&] {
    if (repository_.tariffOverlaps(tariff.adcode, tariff.effectiveFrom,
                                   tariff.effectiveTo)) {
      error = core::domain::ErrorCode::ValidationFailed;
      return;
    }
    repository_.addTariffVersion(tariff);
    repository_.addAuditEvent(AuditEvent{
        actorAdminId, "TARIFF_CREATED", "REGION", tariff.adcode, reason,
        unixSeconds(now)});
  });
  if (error != core::domain::ErrorCode::Ok)
    return {error, std::nullopt};
  return {core::domain::ErrorCode::Ok, tariff};
}

ServiceResult<PriceAdjustment> AdminStationService::createPriceAdjustment(
    const std::int64_t actorAdminId, const PriceAdjustment &draft,
    const std::string &reason, const std::chrono::system_clock::time_point now) {
  if (!validReason(reason) || !charging_.station(draft.stationId) ||
      (draft.chargerType != 0 && draft.chargerType != 1) ||
      (draft.source != "ML_APPROVED" && draft.source != "MANUAL") ||
      draft.adjustmentBp < -2000 || draft.adjustmentBp > 2000 ||
      draft.adjustmentBp % 500 != 0 || draft.effectiveFrom < 0 ||
      draft.effectiveTo <= draft.effectiveFrom) {
    return {core::domain::ErrorCode::ValidationFailed, std::nullopt};
  }
  PriceAdjustment adjustment = draft;
  repository_.withTransaction([&] {
    adjustment.id = repository_.addPriceAdjustment(adjustment);
    repository_.addAuditEvent(AuditEvent{
        actorAdminId, "PRICE_ADJUSTMENT", "STATION",
        std::to_string(adjustment.stationId), reason, unixSeconds(now)});
  });
  return {core::domain::ErrorCode::Ok, adjustment};
}

void AdminStationService::completeDueCommands(
    const std::chrono::system_clock::time_point now) {
  const auto nowSeconds = unixSeconds(now);
  repository_.withTransaction([&] {
    for (const auto &command : repository_.dueDeviceCommands(nowSeconds)) {
      DeviceCommand completed = command;
      completed.completedAt = nowSeconds;
      if (auto charger = charging_.charger(command.chargerId)) {
        if (charger->status == ChargerStatus::Restarting) {
          charger->status = ChargerStatus::Idle;
          ++charger->version;
          charging_.saveCharger(*charger);
          charging_.addChargerStatusEvent(
              ChargerStatusEvent{command.chargerId, charger->stationId,
                                 static_cast<int>(ChargerStatus::Restarting),
                                 static_cast<int>(ChargerStatus::Idle),
                                 "RESTART_SUCCEEDED", nowSeconds});
          completed.status = "SUCCEEDED";
          flows_.promoteAvailable(charger->stationId, charger->type, now);
        } else {
          completed.status = "FAILED";
          completed.errorSummary = "设备状态已变化，重启未执行";
        }
      } else {
        completed.status = "FAILED";
        completed.errorSummary = "设备不存在，重启未执行";
      }
      repository_.saveDeviceCommand(completed);
      std::int64_t actorId = 0;
      const auto parsed = std::from_chars(
          command.actorId.data(), command.actorId.data() + command.actorId.size(),
          actorId);
      if (parsed.ec == std::errc{} &&
          parsed.ptr == command.actorId.data() + command.actorId.size() &&
          actorId > 0) {
        repository_.addAuditEvent(AuditEvent{
            actorId,
            completed.status == "SUCCEEDED" ? "RESTART_SUCCEEDED"
                                              : "RESTART_FAILED",
            "CHARGER", std::to_string(command.chargerId),
            completed.errorSummary, nowSeconds});
      }
    }
  });
}

std::int64_t AdminStationService::effectiveAdjustmentBp(
    const std::int64_t stationId, const int chargerType,
    const std::int64_t at) {
  const auto adjustment =
      repository_.effectivePriceAdjustment(stationId, chargerType, at);
  return adjustment ? adjustment->adjustmentBp : 0;
}

} // namespace ncs::core::application
