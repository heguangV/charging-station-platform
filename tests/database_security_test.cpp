#include "server/runtime/startup_checks.h"

#include <QCoreApplication>
#include <QTemporaryFile>

#include <iostream>
#include <stdexcept>

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    using namespace ncs::server::runtime;
    try
    {
        ServerConfig config;
        config.environment = DeploymentEnvironment::Production;
        const auto expectRejected = [&]
        {
            try
            {
                checkDatabaseSecurity(config);
            }
            catch (const ConfigError&)
            {
                return;
            }
            throw std::runtime_error("unsafe database security configuration accepted");
        };
        for (const auto* mode : {"disable", "allow", "prefer", "require", "verify-ca"})
        {
            config.database.postgres.sslMode = mode;
            expectRejected();
        }
        config.database.postgres.sslMode = "verify-full";
        config.database.postgres.sslRootCertificate.clear();
        expectRejected();
        QTemporaryFile ca;
        if (!ca.open())
            throw std::runtime_error("temporary CA path unavailable");
        // This unit checks path policy only; libpq validates the actual CA and peer
        // certificate in the PostgreSQL TLS smoke test. No socket is opened here.
        config.database.postgres.sslRootCertificate = ca.fileName().toStdString();
        checkDatabaseSecurity(config);
        ca.remove();
        expectRejected();
        config.environment = DeploymentEnvironment::Development;
        config.database.postgres.sslMode = "disable";
        checkDatabaseSecurity(config);
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
