#include "server/runtime/server_config.h"

#include "server/runtime/server_config_environment.h"

#include <asio/ip/address.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QUrl>

#include <array>
#include <charconv>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace ncs::server::runtime
{
namespace
{

struct EnvironmentSetting
{
    std::string_view variable;
    std::string_view envFileVariable;
    std::string_view option;
};

// Env files use the same names as the process environment variables, except
// the Tencent geocoding key, which the frontend/backend .env documents as
// TENCENT_MAP_SERVER_KEY (docs/tencent-map-setup.md).
constexpr std::array<EnvironmentSetting, 22> environmentSettings{{
    {"NCS_ENVIRONMENT", "NCS_ENVIRONMENT", "--environment"},
    {"NCS_LISTEN_ADDRESS", "NCS_LISTEN_ADDRESS", "--listen-address"},
    {"NCS_PORT", "NCS_PORT", "--port"},
    {"NCS_WORKER_THREADS", "NCS_WORKER_THREADS", "--worker-threads"},
    {"NCS_BLOCKING_WORKER_THREADS", "NCS_BLOCKING_WORKER_THREADS", "--blocking-worker-threads"},
    {"NCS_BLOCKING_QUEUE_CAPACITY", "NCS_BLOCKING_QUEUE_CAPACITY", "--blocking-queue-capacity"},
    {"NCS_CHARGE_TIME_SCALE", "NCS_CHARGE_TIME_SCALE", "--charge-time-scale"},
    {"NCS_WEBSOCKET_MAX_CONNECTIONS", "NCS_WEBSOCKET_MAX_CONNECTIONS",
     "--websocket-max-connections"},
    {"NCS_WEBSOCKET_MAX_PAYLOAD", "NCS_WEBSOCKET_MAX_PAYLOAD", "--websocket-max-payload"},
    {"NCS_WEBSOCKET_QUEUE_CAPACITY", "NCS_WEBSOCKET_QUEUE_CAPACITY", "--websocket-queue-capacity"},
    {"NCS_LOG_LEVEL", "NCS_LOG_LEVEL", "--log-level"},
    {"NCS_LOG_DIRECTORY", "NCS_LOG_DIRECTORY", "--log-directory"},
    {"NCS_DATABASE_PATH", "NCS_DATABASE_PATH", "--database-path"},
    {"NCS_TLS_CERTIFICATE", "NCS_TLS_CERTIFICATE", "--tls-certificate"},
    {"NCS_TLS_PRIVATE_KEY", "NCS_TLS_PRIVATE_KEY", "--tls-private-key"},
    {"NCS_ALLOW_INSECURE_HTTP", "NCS_ALLOW_INSECURE_HTTP", "--allow-insecure-http"},
    {"NCS_CORS_ALLOWED_ORIGINS", "NCS_CORS_ALLOWED_ORIGINS", "--cors-allowed-origins"},
    {"NCS_TENCENT_MAP_KEY", "TENCENT_MAP_SERVER_KEY", "--tencent-map-key"},
    {"NCS_DASHBOARD_SNAPSHOT", "NCS_DASHBOARD_SNAPSHOT", "--dashboard-snapshot"},
    {"NCS_PYTHON_EXECUTABLE", "NCS_PYTHON_EXECUTABLE", "--python-executable"},
    {"NCS_ML_WORKER_SCRIPT", "NCS_ML_WORKER_SCRIPT", "--ml-worker-script"},
    {"NCS_ML_MODEL_PATH", "NCS_ML_MODEL_PATH", "--ml-model-path"},
}};

ncs::infrastructure::files::LogLevel parseLogLevel(const std::string_view value,
                                                   const std::string_view source)
{
    using ncs::infrastructure::files::LogLevel;
    if (value == "debug")
    {
        return LogLevel::Debug;
    }
    if (value == "info")
    {
        return LogLevel::Info;
    }
    if (value == "warning")
    {
        return LogLevel::Warning;
    }
    if (value == "error")
    {
        return LogLevel::Error;
    }
    if (value == "critical")
    {
        return LogLevel::Critical;
    }
    throw ConfigError("invalid log level for " + std::string(source));
}

std::string utf8Path(const QString& path)
{
    const auto bytes = path.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

QString pathFromUtf8(const std::string_view value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

std::string normalizePath(const std::string_view value, const std::string_view source)
{
    if (value.empty())
    {
        throw ConfigError("empty filesystem path for " + std::string(source));
    }
    return utf8Path(QFileInfo(pathFromUtf8(value)).absoluteFilePath());
}

std::vector<std::string> parseCorsOrigins(const std::string_view value,
                                          const std::string_view source)
{
    std::vector<std::string> origins;
    std::size_t start = 0;
    while (start <= value.size())
    {
        const std::size_t separator = value.find(',', start);
        const std::string_view item = value.substr(
            start, separator == std::string_view::npos ? value.size() - start : separator - start);
        const QUrl url(pathFromUtf8(item), QUrl::StrictMode);
        const bool validScheme =
            url.scheme() == QStringLiteral("https") || url.scheme() == QStringLiteral("http");
        if (item.empty() || !url.isValid() || !validScheme || url.host().isEmpty() ||
            !url.userInfo().isEmpty() || !url.path().isEmpty() || url.hasQuery() ||
            url.hasFragment())
        {
            throw ConfigError("invalid CORS origin for " + std::string(source));
        }
        const QByteArray normalized = url.toEncoded();
        origins.emplace_back(normalized.constData(), static_cast<std::size_t>(normalized.size()));
        if (separator == std::string_view::npos)
            break;
        start = separator + 1;
    }
    std::sort(origins.begin(), origins.end());
    if (std::adjacent_find(origins.begin(), origins.end()) != origins.end())
    {
        throw ConfigError("duplicate CORS origin for " + std::string(source));
    }
    return origins;
}

unsigned long parseUnsigned(const std::string_view value, const unsigned long minimum,
                            const unsigned long maximum, const std::string_view source)
{
    unsigned long parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (value.empty() || result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        parsed < minimum || parsed > maximum)
    {
        throw ConfigError("invalid numeric value for " + std::string(source));
    }
    return parsed;
}

DeploymentEnvironment parseEnvironment(const std::string_view value, const std::string_view source)
{
    if (value == "development")
    {
        return DeploymentEnvironment::Development;
    }
    if (value == "test")
    {
        return DeploymentEnvironment::Test;
    }
    if (value == "acceptance")
    {
        return DeploymentEnvironment::Acceptance;
    }
    if (value == "production")
    {
        return DeploymentEnvironment::Production;
    }
    throw ConfigError("invalid environment for " + std::string(source));
}

bool parseBoolean(const std::string_view value, const std::string_view source)
{
    if (value == "true")
    {
        return true;
    }
    if (value == "false")
    {
        return false;
    }
    throw ConfigError("invalid boolean value for " + std::string(source));
}

std::string parseListenAddress(const std::string_view value, const std::string_view source)
{
    asio::error_code error;
    const auto address = asio::ip::make_address(value, error);
    if (error || address.is_unspecified() || address.is_multicast())
    {
        throw ConfigError("invalid or uncontrolled listen address for " + std::string(source));
    }
    return address.to_string();
}

void applySetting(ServerConfig& config, const std::string_view option, const std::string_view value,
                  const std::string_view source)
{
    if (option == "--environment")
    {
        config.environment = parseEnvironment(value, source);
    }
    else if (option == "--listen-address")
    {
        config.listenAddress = parseListenAddress(value, source);
    }
    else if (option == "--port")
    {
        config.port = static_cast<std::uint16_t>(parseUnsigned(value, 1, 65535, source));
    }
    else if (option == "--worker-threads")
    {
        config.workerThreads = static_cast<unsigned int>(parseUnsigned(value, 2, 256, source));
    }
    else if (option == "--blocking-worker-threads")
    {
        config.blockingWorkerThreads =
            static_cast<unsigned int>(parseUnsigned(value, 1, 64, source));
    }
    else if (option == "--blocking-queue-capacity")
    {
        config.blockingQueueCapacity =
            static_cast<std::size_t>(parseUnsigned(value, 1, 4096, source));
    }
    else if (option == "--charge-time-scale")
    {
        config.chargeTimeScale = static_cast<std::uint32_t>(
            parseUnsigned(value, 1, std::numeric_limits<std::uint32_t>::max(), source));
    }
    else if (option == "--websocket-max-connections")
    {
        config.websocketMaxConnections =
            static_cast<std::size_t>(parseUnsigned(value, 1, 4096, source));
    }
    else if (option == "--websocket-max-payload")
    {
        config.websocketMaxPayloadBytes =
            static_cast<std::size_t>(parseUnsigned(value, 1024, 1048576, source));
    }
    else if (option == "--websocket-queue-capacity")
    {
        config.websocketQueueCapacity =
            static_cast<std::size_t>(parseUnsigned(value, 16, 4096, source));
    }
    else if (option == "--log-level")
    {
        config.logLevel = parseLogLevel(value, source);
    }
    else if (option == "--log-directory")
    {
        config.logDirectory = normalizePath(value, source);
    }
    else if (option == "--database-path")
    {
        config.databasePath = normalizePath(value, source);
    }
    else if (option == "--tls-certificate")
    {
        config.tlsCertificatePath = normalizePath(value, source);
    }
    else if (option == "--tls-private-key")
    {
        config.tlsPrivateKeyPath = normalizePath(value, source);
    }
    else if (option == "--allow-insecure-http")
    {
        config.allowInsecureHttp = parseBoolean(value, source);
    }
    else if (option == "--cors-allowed-origins")
    {
        config.corsAllowedOrigins = parseCorsOrigins(value, source);
    }
    else if (option == "--tencent-map-key")
    {
        config.tencentMapKey = value;
    }
    else if (option == "--dashboard-snapshot")
    {
        config.dashboardSnapshotPath = normalizePath(value, source);
    }
    else if (option == "--python-executable")
    {
        if (value.empty() || value.size() > 1024)
            throw ConfigError("invalid Python executable for " + std::string(source));
        config.pythonExecutable = std::string(value);
    }
    else if (option == "--ml-worker-script")
    {
        config.mlWorkerScript = normalizePath(value, source);
    }
    else if (option == "--ml-model-path")
    {
        config.mlModelPath = normalizePath(value, source);
    }
    else
    {
        throw ConfigError("unknown option: " + std::string(option));
    }
}

std::optional<StartupAction> requestedAction(const std::vector<std::string>& arguments)
{
    for (const auto& argument : arguments)
    {
        if (argument == "--help" || argument == "-h")
        {
            return StartupAction::ShowHelp;
        }
        if (argument == "--version")
        {
            return StartupAction::ShowVersion;
        }
    }
    return std::nullopt;
}

std::pair<std::string_view, std::optional<std::string_view>>
splitOption(const std::string_view argument)
{
    const auto equals = argument.find('=');
    if (equals == std::string_view::npos)
    {
        return {argument, std::nullopt};
    }
    return {argument.substr(0, equals), argument.substr(equals + 1)};
}

EnvironmentLookup processEnvironment()
{
    return [](const std::string_view name) -> std::optional<std::string>
    {
        const std::string variable(name);
        if (const char* value = std::getenv(variable.c_str()))
        {
            return std::string(value);
        }
        return std::nullopt;
    };
}

} // namespace

ServerConfig::ServerConfig()
{
    QDir baseDirectory = QDir::current();
    if (QCoreApplication::instance())
    {
        // Server defaults must not depend on the shell's launch directory. In a
        // source build, walk from the executable back to the project assets; in
        // an installed layout, fall back to the executable directory itself.
        QDir probe(QCoreApplication::applicationDirPath());
        baseDirectory = probe;
        for (int depth = 0; depth < 8; ++depth)
        {
            if (QFileInfo(probe.filePath(QStringLiteral("ml/worker.py"))).isFile())
            {
                baseDirectory = probe;
                assetDirectoryFound = true;
                break;
            }
            if (!probe.cdUp())
                break;
        }
    }
    assetDirectory = utf8Path(QFileInfo(baseDirectory.absolutePath()).absoluteFilePath());
    const QDir secretDirectory(baseDirectory.filePath(QStringLiteral("secrets")));
    logDirectory =
        utf8Path(QFileInfo(baseDirectory.filePath(QStringLiteral("logs"))).absoluteFilePath());
    databasePath =
        utf8Path(QFileInfo(baseDirectory.filePath(QStringLiteral("data/charge_platform.db")))
                     .absoluteFilePath());
    tlsCertificatePath = utf8Path(
        QFileInfo(secretDirectory.filePath(QStringLiteral("ncs-dev-cert.pem"))).absoluteFilePath());
    tlsPrivateKeyPath = utf8Path(
        QFileInfo(secretDirectory.filePath(QStringLiteral("ncs-dev-key.pem"))).absoluteFilePath());
    dashboardSnapshotPath = utf8Path(QFileInfo(baseDirectory.filePath(QStringLiteral(
                                                   "apps/dashboard/public/data/dashboard.json")))
                                         .absoluteFilePath());
    mlWorkerScript = utf8Path(
        QFileInfo(baseDirectory.filePath(QStringLiteral("ml/worker.py"))).absoluteFilePath());
    mlModelPath =
        utf8Path(QFileInfo(baseDirectory.filePath(QStringLiteral("ml/models/load_rf.pkl")))
                     .absoluteFilePath());
}

bool ServerConfig::demoCredentialsEnabled() const
{
    return environment == DeploymentEnvironment::Development;
}

StartupOptions parseStartupOptions(const std::vector<std::string>& arguments,
                                   const EnvironmentLookup& environmentLookup)
{
    StartupOptions result;
    if (const auto action = requestedAction(arguments))
    {
        result.action = *action;
        return result;
    }

    // Layered configuration: environment-file entries are defaults, process
    // environment variables override them, and command-line values win.
    const auto environmentFile = detail::loadEnvironmentFile(result.config, environmentLookup);
    bool timeScaleExplicit = false;
    if (environmentFile)
    {
        for (const auto& setting : environmentSettings)
        {
            const auto found = environmentFile->find(std::string(setting.envFileVariable));
            if (found == environmentFile->end() || found->second.empty())
                continue;
            applySetting(result.config, setting.option, found->second, setting.envFileVariable);
            if (setting.option == std::string_view("--charge-time-scale"))
                timeScaleExplicit = true;
        }
    }
    for (const auto& setting : environmentSettings)
    {
        if (const auto value = environmentLookup(setting.variable))
        {
            applySetting(result.config, setting.option, *value, setting.variable);
            if (setting.option == std::string_view("--charge-time-scale"))
                timeScaleExplicit = true;
        }
    }

    std::unordered_set<std::string> suppliedOptions;
    for (std::size_t index = 0; index < arguments.size(); ++index)
    {
        const std::string_view argument = arguments[index];
        if (argument.rfind("--", 0) != 0)
        {
            throw ConfigError("unexpected positional argument");
        }

        const auto [option, inlineValue] = splitOption(argument);
        if (!suppliedOptions.insert(std::string(option)).second)
        {
            throw ConfigError("duplicate option: " + std::string(option));
        }
        if (option == "--charge-time-scale")
            timeScaleExplicit = true;

        std::string_view value;
        if (inlineValue)
        {
            value = *inlineValue;
        }
        else
        {
            if (++index >= arguments.size() ||
                std::string_view(arguments[index]).rfind("--", 0) == 0)
            {
                throw ConfigError("missing value for " + std::string(option));
            }
            value = arguments[index];
        }
        applySetting(result.config, option, value, option);
    }

    if (!timeScaleExplicit)
    {
        // The charging clock runs 60x in development so demos complete quickly;
        // other environments must not inherit that acceleration unless it is
        // explicitly requested.
        result.config.chargeTimeScale =
            result.config.environment == DeploymentEnvironment::Development ? 60 : 1;
    }

    if (result.config.demoCredentialsEnabled())
    {
        asio::error_code error;
        const auto address = asio::ip::make_address(result.config.listenAddress, error);
        if (error || !address.is_loopback())
        {
            throw ConfigError("development demo credentials require a loopback listen address");
        }
    }

    if (result.config.allowInsecureHttp)
    {
        asio::error_code error;
        const auto address = asio::ip::make_address(result.config.listenAddress, error);
        if (result.config.environment != DeploymentEnvironment::Development || error ||
            !address.is_loopback())
        {
            throw ConfigError("insecure HTTP requires development and a loopback listen address");
        }
    }

    return result;
}

StartupOptions parseStartupOptions(const int argc, char* const argv[])
{
    std::vector<std::string> arguments;
    arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int index = 1; index < argc; ++index)
    {
        arguments.emplace_back(argv[index]);
    }
    return parseStartupOptions(arguments, processEnvironment());
}

void validateTlsFiles(const ServerConfig& config)
{
    const QFileInfo certificate(pathFromUtf8(config.tlsCertificatePath));
    const QFileInfo privateKey(pathFromUtf8(config.tlsPrivateKeyPath));
    if (!certificate.exists() || !certificate.isFile() || !certificate.isReadable())
    {
        throw ConfigError("TLS certificate is missing or unreadable");
    }
    if (!privateKey.exists() || !privateKey.isFile() || !privateKey.isReadable())
    {
        throw ConfigError("TLS private key is missing or unreadable");
    }
    if (certificate.canonicalFilePath() == privateKey.canonicalFilePath())
    {
        throw ConfigError("TLS certificate and private key must be different files");
    }
}

std::string_view environmentName(const DeploymentEnvironment environment)
{
    switch (environment)
    {
    case DeploymentEnvironment::Development:
        return "development";
    case DeploymentEnvironment::Test:
        return "test";
    case DeploymentEnvironment::Acceptance:
        return "acceptance";
    case DeploymentEnvironment::Production:
        return "production";
    }
    return "unknown";
}

std::string startupHelp()
{
    return R"(Usage: ncs_server [options]

Options:
  --environment <development|test|acceptance|production>
  --listen-address <numeric-ip>   Rejects wildcard and multicast addresses
  --port <1-65535>
  --worker-threads <2-256>
  --blocking-worker-threads <1-64>
  --blocking-queue-capacity <1-4096>
  --charge-time-scale <positive-integer>
  --websocket-max-connections <1-4096>  WebSocket peer capacity (default 100)
  --websocket-max-payload <1024-1048576>  Max inbound frame bytes (default 65536)
  --websocket-queue-capacity <16-4096>  Per-peer frame window (default 256)
  --log-level <debug|info|warning|error|critical>
  --log-directory <path>
  --database-path <sqlite-path>
  --dashboard-snapshot <path>  Atomic offline dashboard snapshot destination
  --python-executable <path-or-name>
  --ml-worker-script <path>
  --ml-model-path <path>
  --tls-certificate <pem-path>
  --tls-private-key <pem-path>
  --allow-insecure-http <true|false>  Development loopback only (default false)
  --cors-allowed-origins <origin,...>
  --tencent-map-key <key>         Server-side geocoding/route key; empty enables fallback
  --help, -h
  --version

Environment variables:
  NCS_ENVIRONMENT, NCS_LISTEN_ADDRESS, NCS_PORT,
  NCS_WORKER_THREADS, NCS_BLOCKING_WORKER_THREADS,
  NCS_BLOCKING_QUEUE_CAPACITY, NCS_CHARGE_TIME_SCALE,
  NCS_WEBSOCKET_MAX_CONNECTIONS, NCS_WEBSOCKET_MAX_PAYLOAD,
  NCS_WEBSOCKET_QUEUE_CAPACITY,
  NCS_LOG_LEVEL, NCS_LOG_DIRECTORY, NCS_DATABASE_PATH,
  NCS_TLS_CERTIFICATE, NCS_TLS_PRIVATE_KEY, NCS_ALLOW_INSECURE_HTTP,
  NCS_CORS_ALLOWED_ORIGINS, NCS_TENCENT_MAP_KEY

Environment files:
  An environment file supplies default values. The file named by NCS_ENV_FILE
  is used when that variable is set (a missing or unreadable file is a hard
  error); otherwise <asset-directory>/.env is loaded when present. Entries
  use the same names as the environment variables above, except the Tencent
  geocoding key, which is written TENCENT_MAP_SERVER_KEY in the file.

Command-line values override environment variables, which override
environment-file entries. Development defaults are
127.0.0.1:8443, 2 event workers, 2 blocking workers, blocking queue capacity
64, charge time scale 60, WebSocket capacity 100 / 64 KiB payload /
256-frame window, no cross-origin access, INFO logs in logs/,
SQLite data in data/charge_platform.db, and
certificate files secrets/ncs-dev-cert.pem and secrets/ncs-dev-key.pem.
Insecure HTTP is disabled by default and is accepted only when explicitly
enabled in development on numeric loopback; all other combinations fail.
Development demo credentials require a loopback listen address. Default file
paths are resolved from the deployed executable/project asset directory.
)";
}

std::string_view startupVersion()
{
#ifdef NCS_SERVER_VERSION
    return NCS_SERVER_VERSION;
#else
    return "unknown";
#endif
}

} // namespace ncs::server::runtime
