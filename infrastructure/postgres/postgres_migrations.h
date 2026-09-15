#pragma once

#include "infrastructure/postgres/postgres_config.h"

class QSqlDatabase;

namespace ncs::infrastructure::postgres
{
// Caller owns the transaction and holds advisory lock 56435350474.
// Import mode skips the full demo generator; V1/V6 bootstrap rows remain.
void applyPostgresMigrations(QSqlDatabase* database, const PostgresConfig& config,
                             bool generateDemo = true);
} // namespace ncs::infrastructure::postgres
