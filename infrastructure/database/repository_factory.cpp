#include "infrastructure/database/repository_factory.h"

#include "infrastructure/postgres/postgres_repository.h"

#include <stdexcept>

namespace ncs::infrastructure::database
{

std::unique_ptr<PlatformRepository> makeRepository(const RepositoryConfig& config)
{
    if (config.driver != "postgresql")
        throw std::invalid_argument("unsupported database driver");
    return std::make_unique<postgres::PostgresRepository>(config.postgres);
}

} // namespace ncs::infrastructure::database
