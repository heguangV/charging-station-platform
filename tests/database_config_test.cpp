#include "server/runtime/server_config.h"
#include "server/runtime/server_config_database.h"

#include <QCoreApplication>
#include <QFileInfo>

#include <iostream>
#include <stdexcept>
#include <utility>

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    using namespace ncs::server::runtime;
    try
    {
        ServerConfig config;
        const auto apply = [&](std::string_view option, std::string_view value)
        {
            if (!detail::applyDatabaseSetting(config, option, value, option))
                throw std::runtime_error("database option not handled");
        };
        apply("--database-driver", "postgresql");
        apply("--database-host", "db.example.test");
        apply("--database-port", "65535");
        apply("--database-name", "ncs_test");
        apply("--database-user", "ncs_test_user");
        apply("--database-password", "");
        apply("--database-connect-timeout", "60");
        apply("--database-pool-size", "64");
        apply("--pg-dump", "/tools/pg_dump");
        apply("--pg-restore", "/tools/pg_restore");
        const auto& pg = config.database.postgres;
        if (config.database.driver != "postgresql" || pg.host != "db.example.test" ||
            pg.port != 65535 || pg.database != "ncs_test" || pg.user != "ncs_test_user" ||
            !pg.password.empty() || pg.connectTimeoutSeconds != 60 || pg.poolSize != 64 ||
            pg.pgDumpExecutable != "/tools/pg_dump" ||
            pg.pgRestoreExecutable != "/tools/pg_restore")
            throw std::runtime_error("database option assignment mismatch");
        for (const auto* mode :
             {"disable", "allow", "prefer", "require", "verify-ca", "verify-full"})
        {
            apply("--database-sslmode", mode);
            if (pg.sslMode != mode)
                throw std::runtime_error("SSL mode parsing mismatch");
        }
        for (const auto* option :
             {"--database-ssl-root-cert", "--database-migrations", "--database-backup-directory"})
            apply(option, "relative-database-path");
        const auto expectedPath =
            QFileInfo(QStringLiteral("relative-database-path")).absoluteFilePath().toStdString();
        if (pg.sslRootCertificate != expectedPath || pg.migrationsDirectory != expectedPath ||
            pg.backupDirectory != expectedPath)
            throw std::runtime_error("database path normalization mismatch");

        const std::pair<std::string, std::string> invalid[]{
            {"--database-driver", "sqlite"},
            {"--database-host", ""},
            {"--database-host", std::string(256, 'x')},
            {"--database-port", "0"},
            {"--database-port", "65536"},
            {"--database-port", "5432x"},
            {"--database-port", "-1"},
            {"--database-port", " 5432"},
            {"--database-name", ""},
            {"--database-name", std::string(64, 'x')},
            {"--database-user", ""},
            {"--database-user", std::string(64, 'x')},
            {"--database-password", std::string(1025, 'x')},
            {"--database-sslmode", "invalid"},
            {"--database-ssl-root-cert", ""},
            {"--database-migrations", ""},
            {"--database-backup-directory", ""},
            {"--database-connect-timeout", "0"},
            {"--database-connect-timeout", "61"},
            {"--database-pool-size", "0"},
            {"--database-pool-size", "65"},
            {"--pg-dump", ""},
            {"--pg-dump", std::string(1025, 'x')},
            {"--pg-restore", ""},
            {"--pg-restore", std::string(1025, 'x')},
        };
        for (const auto& [option, value] : invalid)
        {
            bool rejected = false;
            try
            {
                apply(option, value);
            }
            catch (const ConfigError&)
            {
                rejected = true;
            }
            if (!rejected)
                throw std::runtime_error("invalid database option accepted: " + option);
        }
        if (detail::applyDatabaseSetting(config, "--tls-certificate", "", "test") ||
            detail::applyDatabaseSetting(config, "--database-unknown", "", "test"))
            throw std::runtime_error("unknown option incorrectly consumed");
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
