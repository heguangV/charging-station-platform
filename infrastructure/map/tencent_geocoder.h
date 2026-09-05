#pragma once

#include "core/application/station_service.h"

#include <QString>

namespace ncs::infrastructure::map {

// Tencent Maps WebService geocoder. Any failure — missing key, network error,
// timeout or non-zero API status — resolves to nullopt so callers fall back to
// preset coordinates and Haversine distances.
class TencentGeocoder final : public core::application::Geocoder {
public:
    explicit TencentGeocoder(QString serverKey);

    std::optional<Location> resolve(const std::string &keyword) override;

private:
    QString serverKey_;
};

} // namespace ncs::infrastructure::map
