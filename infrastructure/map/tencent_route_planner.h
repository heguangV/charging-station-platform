#pragma once

#include "core/application/navigation_service.h"

#include <QByteArray>
#include <QString>

namespace ncs::infrastructure::map
{

class TencentRoutePlanner final : public core::application::RoutePlanner
{
  public:
    explicit TencentRoutePlanner(QString serverKey);

    std::optional<core::application::PlannedRoute>
    plan(core::application::RoutePoint origin, core::application::RoutePoint destination,
         core::application::TravelMode mode) override;

    static std::optional<core::application::PlannedRoute>
    parseResponse(const QByteArray& payload, core::application::TravelMode mode);

  private:
    QString serverKey_;
};

} // namespace ncs::infrastructure::map
