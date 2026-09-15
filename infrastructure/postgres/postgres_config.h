#pragma once

#include <cstdint>
#include <string>

namespace ncs::infrastructure::postgres
{

// Connection settings are kept as separate fields so credentials never need
// to be embedded in, logged as, or parsed from a database URL.
struct PostgresConfig
{
    std::string host = "127.0.0.1";
    std::uint16_t port = 5432;
    std::string database = "ncs";
    std::string user = "ncs";
    std::string password;
    std::string sslMode = "prefer";
    std::string sslRootCertificate;
    std::uint32_t connectTimeoutSeconds = 5;
    std::size_t poolSize = 4;
    std::string migrationsDirectory;
    std::string backupDirectory;
    std::string pgDumpExecutable = "pg_dump";
    std::string pgRestoreExecutable = "pg_restore";
};

} // namespace ncs::infrastructure::postgres
