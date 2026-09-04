#include "core/application/charging_repository.h"
#include "core/application/station_service.h"

#include <iostream>
#include <string_view>

namespace {

class FixedGeocoder final : public ncs::core::application::Geocoder {
public:
  explicit FixedGeocoder(std::optional<Location> location)
      : location_(location) {}

  std::optional<Location> resolve(const std::string &) override {
    return location_;
  }

private:
  std::optional<Location> location_;
};

} // namespace

int main() {
  using namespace ncs::core::application;
  InMemoryChargingRepository repository;
  const auto now =
      std::chrono::system_clock::time_point(std::chrono::seconds(1788500000));

  FixedGeocoder successful(Geocoder::Location{39908372, 116457658});
  StationService located(repository, successful);
  const auto resolved = located.nearbyStations(std::nullopt, std::nullopt,
                                               "北京市朝阳区建国门外大街",
                                               std::nullopt, 1, 20, now);
  if (resolved.locationFallback || resolved.total != 3 ||
      resolved.items.empty() || resolved.items.front().code != "CBD") {
    std::cerr << "FAIL: a geocoded address must sort all stations from the "
                 "resolved origin\n";
    return 1;
  }

  FixedGeocoder failing(std::nullopt);
  StationService fallback(repository, failing);
  const auto degraded = fallback.nearbyStations(
      std::nullopt, std::nullopt, "无法解析的地址", std::nullopt, 1, 20, now);
  if (!degraded.locationFallback || degraded.total != 3) {
    std::cerr << "FAIL: geocoding failure must retain all stations at the "
                 "preset origin\n";
    return 1;
  }
  return 0;
}
