#pragma once

#include "infrastructure/files/structured_logger.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ncs::server::runtime
{

enum class DeploymentEnvironment
{
    Development,
    Test,
    Acceptance,
    Production,
};

struct ServerConfig
{
    ServerConfig();

    DeploymentEnvironment environment = DeploymentEnvironment::Development;
    std::string listenAddress = "127.0.0.1";
    std::uint16_t port = 8443;
    unsigned int workerThreads = 2;
    unsigned int blockingWorkerThreads = 2;
    std::size_t blockingQueueCapacity = 64;
    std::uint32_t chargeTimeScale = 60;
    // True when the asset-directory probe found ml/worker.py at or above the
    // executable; false falls back to the executable directory and should
    // surface a startup warning.
    bool assetDirectoryFound = false;
    // Absolute path of the asset directory (the repository root in source
    // builds, the executable directory in installed layouts). The implicit
    // .env candidate lives here.
    std::string assetDirectory;
    std::size_t websocketMaxConnections = 100; // NFR-P-05
    std::size_t websocketMaxPayloadBytes = 64 * 1024;
    std::size_t websocketQueueCapacity = 256;
    ncs::infrastructure::files::LogLevel logLevel = ncs::infrastructure::files::LogLevel::Info;
    std::string logDirectory;
    std::string databasePath;
    std::string tlsCertificatePath;
    std::string tlsPrivateKeyPath;
    // Explicit opt-in for same-host development only. Parsing and startup
    // checks both reject this mode outside development or off loopback.
    bool allowInsecureHttp = false;
    std::vector<std::string> corsAllowedOrigins;
    std::string tencentMapKey;
    std::string dashboardSnapshotPath;
    std::string pythonExecutable = "python3";
    std::string mlWorkerScript;
    std::string mlModelPath;

    bool demoCredentialsEnabled() const;
};

enum class StartupAction
{
    Run,
    ShowHelp,
    ShowVersion,
    BootstrapOwner,
};

struct StartupOptions
{
    ServerConfig config;
    StartupAction action = StartupAction::Run;
    // Username supplied to --bootstrap-owner (StartupAction::BootstrapOwner).
    std::string bootstrapOwnerUsername;
};

class ConfigError final : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

using EnvironmentLookup = std::function<std::optional<std::string>(std::string_view name)>;

StartupOptions parseStartupOptions(const std::vector<std::string>& arguments,
                                   const EnvironmentLookup& environmentLookup);
StartupOptions parseStartupOptions(int argc, char* const argv[]);
void validateTlsFiles(const ServerConfig& config);

std::string_view environmentName(DeploymentEnvironment environment);
std::string startupHelp();
std::string_view startupVersion();

} // namespace ncs::server::runtime
