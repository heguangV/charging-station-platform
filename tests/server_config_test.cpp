#include "server/runtime/server_config.h"
#include "server/runtime/startup_checks.h"

#include <QDir>
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using ncs::server::runtime::ConfigError;
using ncs::server::runtime::DeploymentEnvironment;
using ncs::server::runtime::EnvironmentLookup;
using ncs::server::runtime::StartupAction;
using ncs::infrastructure::files::LogLevel;
using ncs::server::runtime::parseStartupOptions;
using ncs::server::runtime::runStartupChecks;
using ncs::server::runtime::validateTlsFiles;

std::string utf8Path(const QString &path)
{
    const auto bytes = path.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

EnvironmentLookup lookupFor(const std::unordered_map<std::string, std::string> &values)
{
    return [&values](const std::string_view name) -> std::optional<std::string> {
        const auto found = values.find(std::string(name));
        if (found == values.end()) {
            return std::nullopt;
        }
        return found->second;
    };
}

class TestRunner {
public:
    void check(const bool condition, const std::string_view message)
    {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            ++failures_;
        }
    }

    void expectConfigError(
        const std::function<void()> &operation,
        const std::string_view message)
    {
        try {
            operation();
            check(false, message);
        } catch (const ConfigError &) {
        }
    }

    int result() const
    {
        return failures_ == 0 ? 0 : 1;
    }

private:
    int failures_ = 0;
};

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    TestRunner tests;
    const std::unordered_map<std::string, std::string> emptyEnvironment;

    const auto defaults = parseStartupOptions({}, lookupFor(emptyEnvironment));
    tests.check(defaults.action == StartupAction::Run, "default action is run");
    tests.check(
        defaults.config.environment == DeploymentEnvironment::Development,
        "default environment is development");
    tests.check(defaults.config.listenAddress == "127.0.0.1", "default address is loopback");
    tests.check(defaults.config.port == 8443, "default HTTPS port is 8443");
    tests.check(defaults.config.workerThreads == 2, "default worker count is two");
    tests.check(
        defaults.config.blockingWorkerThreads == 2
            && defaults.config.blockingQueueCapacity == 64,
        "default blocking work pool is bounded");
    tests.check(defaults.config.corsAllowedOrigins.empty(), "CORS is denied by default");
    tests.check(defaults.config.chargeTimeScale == 60, "default charge time scale is 60");
    const auto productionDefaults = parseStartupOptions(
        {"--environment", "production"}, lookupFor(emptyEnvironment));
    tests.check(
        productionDefaults.config.chargeTimeScale == 1,
        "non-development environments default to a real-time charge clock");
    const auto productionExplicit = parseStartupOptions(
        {"--environment", "production", "--charge-time-scale", "60"},
        lookupFor(emptyEnvironment));
    tests.check(
        productionExplicit.config.chargeTimeScale == 60,
        "an explicit time scale still overrides the environment default");
    tests.check(
        defaults.config.websocketMaxConnections == 100
            && defaults.config.websocketMaxPayloadBytes == 64 * 1024
            && defaults.config.websocketQueueCapacity == 256,
        "default WebSocket limits are 100 peers, 64 KiB payload and a 256-frame window");
    tests.check(defaults.config.demoCredentialsEnabled(), "development allows demo credentials");
    tests.check(defaults.config.logLevel == LogLevel::Info, "default log level is info");
    tests.check(
        defaults.config.logDirectory.find("logs") != std::string::npos,
        "default log directory is logs");
    tests.check(
        defaults.config.databasePath.find("charge_platform.db") != std::string::npos,
        "default database path is configured");
    tests.check(
        defaults.config.dashboardSnapshotPath.find("dashboard.json") != std::string::npos
            && defaults.config.mlWorkerScript.find("worker.py") != std::string::npos
            && defaults.config.mlModelPath.find("load_rf.pkl") != std::string::npos,
        "default Dashboard and ML paths are configured");
    QTemporaryDir unrelatedWorkingDirectory;
    const QString originalWorkingDirectory = QDir::currentPath();
    tests.check(unrelatedWorkingDirectory.isValid() &&
                    QDir::setCurrent(unrelatedWorkingDirectory.path()),
                "an unrelated launch directory is available");
    const auto relocatedDefaults =
        parseStartupOptions({}, lookupFor(emptyEnvironment));
    QDir::setCurrent(originalWorkingDirectory);
    tests.check(relocatedDefaults.config.databasePath ==
                        defaults.config.databasePath &&
                    relocatedDefaults.config.mlWorkerScript ==
                        defaults.config.mlWorkerScript,
                "default data and asset paths do not depend on the shell cwd");

    const std::unordered_map<std::string, std::string> environment{{
        {"NCS_ENVIRONMENT", "test"},
        {"NCS_LISTEN_ADDRESS", "127.0.0.2"},
        {"NCS_PORT", "9000"},
        {"NCS_WORKER_THREADS", "4"},
        {"NCS_BLOCKING_WORKER_THREADS", "3"},
        {"NCS_BLOCKING_QUEUE_CAPACITY", "96"},
        {"NCS_CHARGE_TIME_SCALE", "120"},
        {"NCS_WEBSOCKET_MAX_CONNECTIONS", "150"},
        {"NCS_WEBSOCKET_MAX_PAYLOAD", "131072"},
        {"NCS_WEBSOCKET_QUEUE_CAPACITY", "512"},
        {"NCS_LOG_LEVEL", "warning"},
        {"NCS_LOG_DIRECTORY", "env-logs"},
        {"NCS_DATABASE_PATH", "env-data.db"},
        {"NCS_TLS_CERTIFICATE", "env-cert.pem"},
        {"NCS_TLS_PRIVATE_KEY", "env-key.pem"},
        {"NCS_CORS_ALLOWED_ORIGINS", "https://dashboard.local,http://127.0.0.1:5173"},
        {"NCS_DASHBOARD_SNAPSHOT", "env-dashboard.json"},
        {"NCS_PYTHON_EXECUTABLE", "python-test"},
        {"NCS_ML_WORKER_SCRIPT", "env-worker.py"},
        {"NCS_ML_MODEL_PATH", "env-model.pkl"},
    }};
    const auto fromEnvironment = parseStartupOptions({}, lookupFor(environment));
    tests.check(
        fromEnvironment.config.environment == DeploymentEnvironment::Test,
        "environment values are applied");
    tests.check(
        !fromEnvironment.config.demoCredentialsEnabled(),
        "non-development environments reject demo credentials");
    tests.check(fromEnvironment.config.listenAddress == "127.0.0.2", "environment address");
    tests.check(fromEnvironment.config.port == 9000, "environment port");
    tests.check(fromEnvironment.config.workerThreads == 4, "environment worker count");
    tests.check(
        fromEnvironment.config.blockingWorkerThreads == 3
            && fromEnvironment.config.blockingQueueCapacity == 96,
        "environment blocking work limits");
    tests.check(
        fromEnvironment.config.corsAllowedOrigins.size() == 2,
        "environment CORS origins are parsed");
    tests.check(fromEnvironment.config.chargeTimeScale == 120, "environment time scale");
    tests.check(
        fromEnvironment.config.websocketMaxConnections == 150
            && fromEnvironment.config.websocketMaxPayloadBytes == 131072
            && fromEnvironment.config.websocketQueueCapacity == 512,
        "environment WebSocket limits are applied");
    tests.check(fromEnvironment.config.logLevel == LogLevel::Warning, "environment log level");
    tests.check(
        fromEnvironment.config.logDirectory.find("env-logs") != std::string::npos,
        "environment log directory");
    tests.check(
        fromEnvironment.config.databasePath.find("env-data.db") != std::string::npos,
        "environment database path");
    tests.check(
        fromEnvironment.config.tlsCertificatePath.find("env-cert.pem") != std::string::npos,
        "environment certificate path");
    tests.check(
        fromEnvironment.config.tlsPrivateKeyPath.find("env-key.pem") != std::string::npos,
        "environment private key path");
    tests.check(fromEnvironment.config.pythonExecutable == "python-test" &&
                    fromEnvironment.config.dashboardSnapshotPath.find("env-dashboard.json") !=
                        std::string::npos,
                "environment Dashboard and ML process settings are applied");

    // Environment-file layer: entries act as defaults below the process
    // environment, and the Tencent geocoding key is written
    // TENCENT_MAP_SERVER_KEY in the file.
    QTemporaryDir envFileDirectory;
    tests.check(envFileDirectory.isValid(),
                "temporary environment-file directory is available");
    const QString envFilePath = QDir(envFileDirectory.path()).filePath("config.env");
    QFile envFile(envFilePath);
    tests.check(envFile.open(QIODevice::WriteOnly), "environment file can be created");
    envFile.write(
        "# development defaults\n"
        "NCS_PORT=8200\n"
        "export NCS_WORKER_THREADS=5\n"
        "NCS_LOG_LEVEL='debug'\n"
        "NCS_LISTEN_ADDRESS = 127.0.0.9\n"
        "NCS_CHARGE_TIME_SCALE=120\n"
        "TENCENT_MAP_SERVER_KEY=\"file-map-key\"\n"
        "\n"
        "BARE_GARBAGE\n"
        "NOT_A_VALID KEY=ignored\n"
        "TENCENT_MAP_JS_KEY=frontend-key-is-ignored-by-the-server\n"
        "NCS_PORT=8300\n"
        "NCS_DATABASE_PATH=\n");
    envFile.close();
    const auto withEnvFile = parseStartupOptions(
        {}, lookupFor({{"NCS_ENV_FILE", utf8Path(envFilePath)}}));
    tests.check(withEnvFile.config.port == 8200,
                "environment-file values are applied");
    tests.check(withEnvFile.config.workerThreads == 5,
                "environment-file export prefix is accepted");
    tests.check(withEnvFile.config.logLevel == LogLevel::Debug,
                "environment-file quoted values are parsed");
    tests.check(withEnvFile.config.listenAddress == "127.0.0.9",
                "environment-file spacing around = is tolerated");
    tests.check(withEnvFile.config.chargeTimeScale == 120,
                "environment-file charge time scale counts as explicit");
    tests.check(withEnvFile.config.tencentMapKey == "file-map-key",
                "TENCENT_MAP_SERVER_KEY maps to the Tencent geocoding key");
    tests.check(
        withEnvFile.config.databasePath.find("charge_platform.db") != std::string::npos,
        "empty environment-file values keep the built-in default");
    tests.check(withEnvFile.config.port == 8200,
                "the first duplicate environment-file entry wins");

    const std::unordered_map<std::string, std::string> fileThenEnvironment{{
        {"NCS_ENV_FILE", utf8Path(envFilePath)},
        {"NCS_PORT", "9000"},
        {"NCS_TENCENT_MAP_KEY", "env-map-key"},
    }};
    const auto envBeatsFile = parseStartupOptions({}, lookupFor(fileThenEnvironment));
    tests.check(envBeatsFile.config.port == 9000,
                "process environment overrides the environment file");
    tests.check(envBeatsFile.config.tencentMapKey == "env-map-key",
                "NCS_TENCENT_MAP_KEY overrides the file's TENCENT_MAP_SERVER_KEY");
    tests.check(envBeatsFile.config.workerThreads == 5,
                "file entries not overridden by the environment still apply");
    const auto cliBeatsAll = parseStartupOptions(
        {"--port", "8443"}, lookupFor(fileThenEnvironment));
    tests.check(cliBeatsAll.config.port == 8443,
                "command line overrides environment and file values");

    tests.expectConfigError(
        [&] {
            parseStartupOptions(
                {}, lookupFor({{"NCS_ENV_FILE", utf8Path(
                                    QDir(envFileDirectory.path())
                                        .filePath("missing.env"))}}));
        },
        "a missing NCS_ENV_FILE is rejected");
    QFile oversizedFile(QDir(envFileDirectory.path()).filePath("oversized.env"));
    tests.check(oversizedFile.open(QIODevice::WriteOnly),
                "oversized environment file can be created");
    oversizedFile.write(QByteArray(64 * 1024 + 1, '#'));
    oversizedFile.close();
    tests.expectConfigError(
        [&] {
            parseStartupOptions(
                {}, lookupFor({{"NCS_ENV_FILE", utf8Path(oversizedFile.fileName())}}));
        },
        "an oversized environment file is rejected");

    const std::vector<std::string> commandLine{
        "--environment=acceptance",
        "--listen-address", "::1",
        "--port", "8443",
        "--worker-threads=8",
        "--blocking-worker-threads=4",
        "--blocking-queue-capacity", "128",
        "--charge-time-scale", "180",
        "--websocket-max-connections", "200",
        "--websocket-max-payload=262144",
        "--websocket-queue-capacity", "384",
        "--log-level=debug",
        "--log-directory", "cli-logs",
        "--database-path", "cli-data.db",
        "--tls-certificate", "cli-cert.pem",
        "--tls-private-key=cli-key.pem",
        "--cors-allowed-origins", "https://dashboard.example",
        "--dashboard-snapshot", "cli-dashboard.json",
        "--python-executable", "python-cli",
        "--ml-worker-script", "cli-worker.py",
        "--ml-model-path", "cli-model.pkl",
    };
    const auto overridden = parseStartupOptions(commandLine, lookupFor(environment));
    tests.check(
        overridden.config.environment == DeploymentEnvironment::Acceptance,
        "command line overrides environment mode");
    tests.check(overridden.config.listenAddress == "::1", "command line overrides address");
    tests.check(overridden.config.port == 8443, "command line overrides port");
    tests.check(overridden.config.workerThreads == 8, "command line overrides workers");
    tests.check(
        overridden.config.blockingWorkerThreads == 4
            && overridden.config.blockingQueueCapacity == 128,
        "command line overrides blocking work limits");
    tests.check(
        overridden.config.corsAllowedOrigins
            == std::vector<std::string>{"https://dashboard.example"},
        "command line overrides CORS origins");
    tests.check(overridden.config.chargeTimeScale == 180, "command line overrides time scale");
    tests.check(
        overridden.config.websocketMaxConnections == 200
            && overridden.config.websocketMaxPayloadBytes == 262144
            && overridden.config.websocketQueueCapacity == 384,
        "command line overrides WebSocket limits");
    tests.check(overridden.config.logLevel == LogLevel::Debug, "command line overrides log level");
    tests.check(
        overridden.config.logDirectory.find("cli-logs") != std::string::npos,
        "command line overrides log directory");
    tests.check(
        overridden.config.databasePath.find("cli-data.db") != std::string::npos,
        "command line overrides database path");
    tests.check(
        overridden.config.tlsCertificatePath.find("cli-cert.pem") != std::string::npos,
        "command line overrides certificate path");
    tests.check(
        overridden.config.tlsPrivateKeyPath.find("cli-key.pem") != std::string::npos,
        "command line overrides private key path");
    tests.check(overridden.config.pythonExecutable == "python-cli" &&
                    overridden.config.mlWorkerScript.find("cli-worker.py") !=
                        std::string::npos,
                "command line overrides ML process settings");

    const auto help = parseStartupOptions(
        {"--help"},
        [](std::string_view) -> std::optional<std::string> {
            throw ConfigError("environment must not be read for help");
        });
    tests.check(help.action == StartupAction::ShowHelp, "help bypasses environment loading");
    const auto version = parseStartupOptions({"--version"}, lookupFor(emptyEnvironment));
    tests.check(version.action == StartupAction::ShowVersion, "version action is supported");

    tests.expectConfigError(
        [&] { parseStartupOptions({"--unknown", "value"}, lookupFor(emptyEnvironment)); },
        "unknown options are rejected");
    tests.expectConfigError(
        [&] { parseStartupOptions({"--port"}, lookupFor(emptyEnvironment)); },
        "missing values are rejected");
    tests.expectConfigError(
        [&] { parseStartupOptions({"--port", "0"}, lookupFor(emptyEnvironment)); },
        "port zero is rejected");
    tests.expectConfigError(
        [&] { parseStartupOptions({"--worker-threads", "1"}, lookupFor(emptyEnvironment)); },
        "Crow worker counts below two are rejected");
    tests.expectConfigError(
        [&] { parseStartupOptions({"--worker-threads", "257"}, lookupFor(emptyEnvironment)); },
        "excessive worker count is rejected");
    tests.expectConfigError(
        [&] { parseStartupOptions({"--websocket-max-connections", "0"}, lookupFor(emptyEnvironment)); },
        "a zero WebSocket peer capacity is rejected");
    tests.expectConfigError(
        [&] { parseStartupOptions({"--websocket-max-connections", "4097"}, lookupFor(emptyEnvironment)); },
        "an excessive WebSocket peer capacity is rejected");
    tests.expectConfigError(
        [&] { parseStartupOptions({"--websocket-max-payload", "1023"}, lookupFor(emptyEnvironment)); },
        "a WebSocket payload limit below 1 KiB is rejected");
    tests.expectConfigError(
        [&] { parseStartupOptions({"--websocket-queue-capacity", "15"}, lookupFor(emptyEnvironment)); },
        "a WebSocket frame window below 16 is rejected");
    tests.expectConfigError(
        [&] { parseStartupOptions(
            {"--blocking-queue-capacity", "0"}, lookupFor(emptyEnvironment)); },
        "empty blocking queue is rejected");
    tests.expectConfigError(
        [&] { parseStartupOptions(
            {"--cors-allowed-origins", "https://good.example/path"},
            lookupFor(emptyEnvironment)); },
        "CORS origins with paths are rejected");
    tests.expectConfigError(
        [&] { parseStartupOptions(
            {"--cors-allowed-origins", "https://duplicate.example,https://duplicate.example"},
            lookupFor(emptyEnvironment)); },
        "duplicate CORS origins are rejected");
    tests.expectConfigError(
        [&] { parseStartupOptions({"--listen-address", "0.0.0.0"}, lookupFor(emptyEnvironment)); },
        "wildcard addresses are rejected");
    tests.expectConfigError(
        [&] { parseStartupOptions({"--listen-address", "not-an-ip"}, lookupFor(emptyEnvironment)); },
        "non-numeric addresses are rejected");
    tests.expectConfigError(
        [&] { parseStartupOptions({"--listen-address", "192.168.1.8"},
                                  lookupFor(emptyEnvironment)); },
        "development demo credentials cannot bind to a non-loopback address");
    tests.expectConfigError(
        [&] { parseStartupOptions({"--log-level", "trace"}, lookupFor(emptyEnvironment)); },
        "unknown log levels are rejected");
    tests.expectConfigError(
        [&] { parseStartupOptions({"--environment", "staging"}, lookupFor(emptyEnvironment)); },
        "unknown environment modes are rejected");
    tests.expectConfigError(
        [&] { parseStartupOptions({"--port=9000", "--port", "9001"}, lookupFor(emptyEnvironment)); },
        "duplicate options are rejected");

    QTemporaryDir tlsDirectory;
    tests.check(tlsDirectory.isValid(), "temporary TLS directory is available");
    const QString certificatePath = QDir(tlsDirectory.path()).filePath("certificate.pem");
    const QString privateKeyPath = QDir(tlsDirectory.path()).filePath("private-key.pem");
    QFile certificate(certificatePath);
    QFile privateKey(privateKeyPath);
    tests.check(certificate.open(QIODevice::WriteOnly), "temporary certificate can be created");
    tests.check(privateKey.open(QIODevice::WriteOnly), "temporary private key can be created");
    certificate.write("test certificate");
    privateKey.write("test private key");
    certificate.close();
    privateKey.close();

    auto tlsConfig = defaults.config;
    tlsConfig.tlsCertificatePath = utf8Path(certificatePath);
    tlsConfig.tlsPrivateKeyPath = utf8Path(privateKeyPath);
    try {
        validateTlsFiles(tlsConfig);
        tests.check(true, "distinct readable TLS files are accepted");
    } catch (const ConfigError &) {
        tests.check(false, "distinct readable TLS files are accepted");
    }
    tests.expectConfigError(
        [&] {
            auto sameFileConfig = tlsConfig;
            sameFileConfig.tlsPrivateKeyPath = sameFileConfig.tlsCertificatePath;
            validateTlsFiles(sameFileConfig);
        },
        "certificate and private key must be distinct files");
#ifdef Q_OS_UNIX
    tests.check(
        QFile::setPermissions(
            privateKeyPath,
            QFileDevice::ReadOwner | QFileDevice::WriteOwner),
        "temporary private key permissions can be restricted");
#endif
    tests.expectConfigError(
        [&] { runStartupChecks(tlsConfig); },
        "invalid PEM is rejected by startup checks");
    tests.check(privateKey.remove(), "temporary private key can be removed");
    tests.expectConfigError(
        [&] { validateTlsFiles(tlsConfig); },
        "missing private key is rejected before Crow starts");

    return tests.result();
}
