#pragma once

#include "core/application/admin_repository.h"
#include "core/application/analytics_service.h"
#include "core/application/business_numbers.h"
#include "core/application/charging_repository.h"
#include "core/application/idempotency_service.h"
#include "core/application/order_review_service.h"
#include "core/application/readiness_probe.h"
#include "core/application/user_account_repository.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace ncs::infrastructure::database
{

// Composition root boundary for the platform's persistence adapter. Keeping the existing nine
// ports avoids a simultaneous business-layer redesign while allowing the concrete database driver
// to be selected and owned in exactly one place.
class PlatformRepository : public core::application::UserAccountRepository,
                           public core::application::WalletMirror,
                           public core::application::ChargingRepository,
                           public core::application::OrderReviewRepository,
                           public core::application::ReadinessProbe,
                           public core::application::BusinessNumberSequenceStore,
                           public core::application::IdempotencyPersistence,
                           public core::application::AdminRepository,
                           public core::application::AnalyticsRepository
{
  public:
    ~PlatformRepository() override = default;

    virtual void ensureDevelopmentAdmin(bool enabled) = 0;
    virtual void refreshReadiness() = 0;
    virtual std::optional<core::application::AdminAccount>
    bootstrapOwnerAccount(std::string_view username, std::string_view passwordHash,
                          std::int64_t at) = 0;
};

} // namespace ncs::infrastructure::database
