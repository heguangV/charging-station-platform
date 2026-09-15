#include "infrastructure/postgres/postgres_migrations.h"
#include "infrastructure/postgres/postgres_repository.h"
#include "infrastructure/postgres/postgres_repository_detail.h"
#include "infrastructure/postgres/postgres_seed.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <utility>

namespace ncs::infrastructure::postgres
{

using namespace detail;

PostgresRepository::PostgresRepository(PostgresConfig config)
    : config_(std::move(config)), connectionSlots_(static_cast<int>(config_.poolSize))
{
    if (config_.migrationsDirectory.empty())
        throw std::invalid_argument("PostgreSQL migrations directory is not configured");
    if (config_.poolSize == 0 || config_.poolSize > 64)
        throw std::invalid_argument("PostgreSQL pool size must be between 1 and 64");
    initialize();
    refreshReadiness();
}

PostgresRepository::~PostgresRepository() = default;

namespace
{

struct Migration
{
    int version;
    const char* name;
    const char* checksum;
    const char* file;
};

constexpr std::array<Migration, 10> migrations{{
    {1, "initial-user-charging", "ncs-pg-v1-initial", "V001__initial_user_charging.sql"},
    {2, "admin-control-plane", "ncs-pg-v2-admin", "V002__admin_control_plane.sql"},
    {3, "charger-restarting-state", "ncs-pg-v3-restarting", "V003__charger_restarting_state.sql"},
    {4, "development-admin-marker", "ncs-pg-v4-demo-admin", "V004__development_admin_marker.sql"},
    {5, "admin-query-indexes", "ncs-pg-v5-admin-indexes", "V005__admin_query_indexes.sql"},
    {6, "dashboard-ml", "ncs-pg-v6-dashboard-ml", "V006__dashboard_ml.sql"},
    {7, "order-analytics-indexes", "ncs-pg-v7-order-analytics",
     "V007__order_analytics_indexes.sql"},
    {8, "full-demo-seed", "ncs-pg-v8-full-demo-seed", "V008__full_demo_seed.sql"},
    {9, "order-review", "ncs-pg-v9-order-review", "V009__order_review.sql"},
    {10, "order-confirmation", "ncs-v10-order-confirmation", "V010__order_confirmation.sql"},
}};

std::string readMigration(const std::string& directory, const char* file)
{
    const std::filesystem::path path = std::filesystem::path(directory) / file;
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("database migration file is missing or unreadable");
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size < 0 || size > 1024 * 1024)
        throw std::runtime_error("database migration file has an invalid size");
    input.seekg(0, std::ios::beg);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void synchronizeIdentity(QSqlDatabase* database, const char* table, const char* column)
{
    const std::string sql = "SELECT setval(pg_get_serial_sequence('" + std::string(table) + "','" +
                            column + "'), COALESCE(MAX(" + column +
                            "), 1), "
                            "MAX(" +
                            column + ") IS NOT NULL) FROM " + table;
    Statement statement(database, sql.c_str());
    if (!statement.row())
        throw std::runtime_error("database identity synchronization failed");
}

} // namespace

void applyPostgresMigrations(QSqlDatabase* database, const PostgresConfig& config,
                             const bool generateDemo)
{
    execute(database, "CREATE TABLE IF NOT EXISTS schema_version("
                      "version INTEGER PRIMARY KEY,name TEXT NOT NULL,checksum TEXT NOT NULL,"
                      "applied_at BIGINT NOT NULL)");

    std::map<int, std::string> applied;
    Statement versions(database, "SELECT version,checksum FROM schema_version ORDER BY version");
    while (versions.row())
        applied.emplace(static_cast<int>(versions.integer(0)), versions.text(1));
    if (!applied.empty() && applied.rbegin()->first > kLatestSchemaVersion)
        throw std::runtime_error("database schema is newer than this server");

    for (const auto& migration : migrations)
    {
        const auto found = applied.find(migration.version);
        if (found != applied.end())
        {
            if (found->second != migration.checksum)
                throw std::runtime_error("database migration checksum mismatch");
            continue;
        }

        const std::string sql = readMigration(config.migrationsDirectory, migration.file);
        if (migration.version == 8)
        {
            if (generateDemo)
            {
                const std::int64_t anchorAt =
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
                applyFullDemoSeed(database, anchorAt);
                constexpr std::array<std::pair<const char*, const char*>, 11> identities{{
                    {"user_account", "id"},
                    {"station", "id"},
                    {"charger", "id"},
                    {"region_tariff", "id"},
                    {"wallet_transaction", "id"},
                    {"flow_event", "id"},
                    {"outbox_event", "id"},
                    {"admin_account", "id"},
                    {"ops_log", "id"},
                    {"price_adjustment", "id"},
                    {"flow_queue", "sequence"},
                }};
                for (const auto& [table, column] : identities)
                    synchronizeIdentity(database, table, column);
            }
        }
        else if (!sql.empty())
        {
            execute(database, sql.c_str());
        }

        Statement marker(
            database, "INSERT INTO schema_version(version,name,checksum,applied_at) VALUES(?,?,?,"
                      "EXTRACT(EPOCH FROM clock_timestamp())::BIGINT)");
        marker.bind(1, migration.version);
        marker.bind(2, migration.name);
        marker.bind(3, migration.checksum);
        marker.execute();
    }
}

// Each schema version and its data commit together, serialized with offline import.
void PostgresRepository::initialize()
{
    Connection connection(config_, &connectionSlots_);
    connection.execute("BEGIN");
    try
    {
        connection.execute("SELECT pg_advisory_xact_lock(56435350474)");
        applyPostgresMigrations(connection.get(), config_);
        connection.execute("COMMIT");
    }
    catch (...)
    {
        try
        {
            connection.execute("ROLLBACK");
        }
        catch (...)
        {
        }
        throw;
    }
}

} // namespace ncs::infrastructure::postgres
