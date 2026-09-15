#pragma once

#include "infrastructure/database/platform_repository.h"
#include "infrastructure/postgres/postgres_config.h"

#include <memory>
#include <string>

namespace ncs::infrastructure::database
{

struct RepositoryConfig
{
    std::string driver = "postgresql";
    postgres::PostgresConfig postgres;
};

std::unique_ptr<PlatformRepository> makeRepository(const RepositoryConfig& config);

} // namespace ncs::infrastructure::database
