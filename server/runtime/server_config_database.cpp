#include "server/runtime/server_config_database.h"

#include "server/runtime/server_config.h"
#include "server/runtime/server_config_values.h"

#include <algorithm>
#include <array>

namespace ncs::server::runtime::detail
{

bool applyDatabaseSetting(ServerConfig& config, const std::string_view option,
                          const std::string_view value, const std::string_view source)
{
    if (option == "--database-driver")
    {
        if (value != "postgresql")
            throw ConfigError("invalid database driver for " + std::string(source));
        config.database.driver = std::string(value);
    }
    else if (option == "--database-host")
    {
        if (value.empty() || value.size() > 255)
            throw ConfigError("invalid database host for " + std::string(source));
        config.database.postgres.host = std::string(value);
    }
    else if (option == "--database-port")
    {
        config.database.postgres.port =
            static_cast<std::uint16_t>(parseUnsigned(value, 1, 65535, source));
    }
    else if (option == "--database-name")
    {
        if (value.empty() || value.size() > 63)
            throw ConfigError("invalid database name for " + std::string(source));
        config.database.postgres.database = std::string(value);
    }
    else if (option == "--database-user")
    {
        if (value.empty() || value.size() > 63)
            throw ConfigError("invalid database user for " + std::string(source));
        config.database.postgres.user = std::string(value);
    }
    else if (option == "--database-password")
    {
        if (value.size() > 1024)
            throw ConfigError("invalid database password for " + std::string(source));
        config.database.postgres.password = std::string(value);
    }
    else if (option == "--database-sslmode")
    {
        constexpr std::array<std::string_view, 6> modes{"disable", "allow",     "prefer",
                                                        "require", "verify-ca", "verify-full"};
        if (std::find(modes.begin(), modes.end(), value) == modes.end())
            throw ConfigError("invalid database SSL mode for " + std::string(source));
        config.database.postgres.sslMode = std::string(value);
    }
    else if (option == "--database-ssl-root-cert")
    {
        config.database.postgres.sslRootCertificate = normalizePath(value, source);
    }
    else if (option == "--database-connect-timeout")
    {
        config.database.postgres.connectTimeoutSeconds =
            static_cast<std::uint32_t>(parseUnsigned(value, 1, 60, source));
    }
    else if (option == "--database-pool-size")
    {
        config.database.postgres.poolSize =
            static_cast<std::size_t>(parseUnsigned(value, 1, 64, source));
    }
    else if (option == "--database-migrations")
    {
        config.database.postgres.migrationsDirectory = normalizePath(value, source);
    }
    else if (option == "--database-backup-directory")
    {
        config.database.postgres.backupDirectory = normalizePath(value, source);
    }
    else if (option == "--pg-dump")
    {
        if (value.empty() || value.size() > 1024)
            throw ConfigError("invalid pg_dump executable for " + std::string(source));
        config.database.postgres.pgDumpExecutable = std::string(value);
    }
    else if (option == "--pg-restore")
    {
        if (value.empty() || value.size() > 1024)
            throw ConfigError("invalid pg_restore executable for " + std::string(source));
        config.database.postgres.pgRestoreExecutable = std::string(value);
    }
    else
    {
        return false;
    }
    return true;
}

} // namespace ncs::server::runtime::detail
