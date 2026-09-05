#include <crow.h>

#include "core/application/admin_account_service.h"
#include "core/application/admin_auth_service.h"
#include "core/application/admin_ops_service.h"
#include "core/application/admin_repository.h"
#include "core/application/admin_station_service.h"
#include "core/application/admin_user_service.h"
#include "core/application/bounded_executor.h"
#include "core/application/business_numbers.h"
#include "core/application/charge_flow_service.h"
#include "core/application/charging_repository.h"
#include "core/application/event_hub.h"
#include "core/application/idempotency_service.h"
#include "core/application/station_service.h"
#include "core/application/wallet_service.h"
#include "infrastructure/files/model_artifact_store.h"
#include "infrastructure/map/tencent_geocoder.h"
#include "infrastructure/map/tencent_route_planner.h"
#include "infrastructure/sqlite/sqlite_repository.h"
#include "server/controller/admin_routes.h"
#include "server/controller/api_routes.h"
#include "server/controller/dashboard_routes.h"
#include "server/controller/flow_routes.h"
#include "server/controller/health_routes.h"
#include "server/controller/ml_routes.h"
#include "server/controller/navigation_routes.h"
#include "server/controller/station_routes.h"
#include "server/controller/user_identity_routes.h"
#include "server/controller/wallet_routes.h"
#include "server/middleware/crow_log_handler.h"
#include "server/middleware/global_exception_handler.h"
#include "server/runtime/ml_process_manager.h"
#include "server/runtime/periodic_scheduler.h"
#include "server/runtime/server_config.h"
#include "server/runtime/startup_checks.h"
#include "server/server_app.h"
#include "server/websocket/outbox_dispatcher.h"
#include "server/websocket/progress_pusher.h"
#include "server/websocket/websocket_routes.h"

#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <memory>
#include <unordered_set>

#include <QCoreApplication>

namespace
{

using ncs::infrastructure::files::LogLevel;
using ncs::infrastructure::files::StructuredLogger;

void logFailureSafely(StructuredLogger* logger, const std::string& message)
{
    if (!logger)
    {
        return;
    }
    try
    {
        logger->log(LogLevel::Error, "server.lifecycle", message);
    }
    catch (...)
    {
    }
}

std::string localServerUrl(const ncs::server::runtime::ServerConfig& config)
{
    const std::string host = config.listenAddress.find(':') == std::string::npos
                                 ? config.listenAddress
                                 : "[" + config.listenAddress + "]";
    return std::string(config.allowInsecureHttp ? "http://" : "https://") + host + ":" +
           std::to_string(config.port);
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication qtApplication(argc, argv);
    std::unique_ptr<StructuredLogger> logger;
    try
    {
        const auto startup = ncs::server::runtime::parseStartupOptions(argc, argv);
        if (startup.action == ncs::server::runtime::StartupAction::ShowHelp)
        {
            std::cout << ncs::server::runtime::startupHelp();
            return 0;
        }
        if (startup.action == ncs::server::runtime::StartupAction::ShowVersion)
        {
            std::cout << "ncs_server " << ncs::server::runtime::startupVersion() << '\n';
            return 0;
        }

        logger = std::make_unique<StructuredLogger>(StructuredLogger::Options{
            startup.config.logDirectory,
            startup.config.logLevel,
            30,
            true,
        });
        ncs::server::middleware::CrowLogHandler crowLogHandler(*logger);
        crow::logger::setHandler(&crowLogHandler);
        crow::logger::setLogLevel(ncs::server::middleware::toCrowLogLevel(startup.config.logLevel));

        logger->log(LogLevel::Info, "server.lifecycle", "startup checks begin");
        if (!startup.config.assetDirectoryFound)
        {
            logger->log(LogLevel::Warning, "server.lifecycle",
                        "project assets not found at or above the executable directory; "
                        "logs, data and snapshots default to the executable directory");
        }
        ncs::server::runtime::runStartupChecks(startup.config);
        logger->log(LogLevel::Info, "server.lifecycle", "startup checks passed");
        if (startup.config.allowInsecureHttp)
        {
            logger->log(LogLevel::Warning, "server.transport",
                        "INSECURE DEVELOPMENT HTTP ENABLED on loopback; traffic is not encrypted");
        }
        ncs::server::ServerApp app;
        auto& requestPolicy =
            app.get_middleware<ncs::server::middleware::RequestPolicyMiddleware>();
        requestPolicy.configure(
            *logger,
            std::unordered_set<std::string>(startup.config.corsAllowedOrigins.begin(),
                                            startup.config.corsAllowedOrigins.end()),
            !startup.config.allowInsecureHttp);
        ncs::server::middleware::installGlobalExceptionHandler(app, *logger);
        ncs::server::controller::ApiRoutes apiRoutes(app);
        ncs::core::application::SessionManager sessions;
        auto hub = std::make_shared<ncs::core::application::EventHub>(
            ncs::core::application::EventHubOptions{
                startup.config.websocketMaxConnections,
                startup.config.websocketQueueCapacity,
                1 << 20,
                std::chrono::seconds(60),
                std::chrono::seconds(30),
                std::chrono::seconds(60),
                [&sessions](const std::string_view token) {
                    return sessions.authenticate(token, std::chrono::system_clock::now())
                        .has_value();
                },
            });
        ncs::core::application::VerificationCodeService verificationCodes(
            startup.config.demoCredentialsEnabled());
        ncs::infrastructure::sqlite::SqliteRepository repository(startup.config.databasePath);
        repository.ensureDevelopmentAdmin(startup.config.demoCredentialsEnabled());
        ncs::server::websocket::OutboxDispatcher outboxDispatcher(repository, hub);
        ncs::core::application::UserIdentityService userIdentity(repository, sessions,
                                                                 verificationCodes);
        ncs::core::application::IdempotencyService idempotency(&repository);
        ncs::server::controller::HealthRoutes healthRoutes(apiRoutes, repository, sessions);

        ncs::core::application::BusinessNumbers businessNumbers(&repository);
        ncs::core::application::WalletService walletService(repository, repository,
                                                            businessNumbers);
        ncs::infrastructure::map::TencentGeocoder geocoder(
            QString::fromStdString(startup.config.tencentMapKey));
        ncs::infrastructure::map::TencentRoutePlanner routePlanner(
            QString::fromStdString(startup.config.tencentMapKey));
        ncs::core::application::NavigationService navigationService(repository, geocoder,
                                                                    routePlanner);
        const auto adjustmentLookup = [&repository](const std::int64_t stationId,
                                                    const int chargerType, const std::int64_t at)
        {
            const auto adjustment = repository.effectivePriceAdjustment(stationId, chargerType, at);
            return adjustment ? adjustment->adjustmentBp : 0LL;
        };
        ncs::core::application::StationService stationService(repository, geocoder,
                                                              adjustmentLookup);
        ncs::core::application::ChargeFlowService chargeFlowService(
            repository, repository, repository, businessNumbers,
            static_cast<int>(startup.config.chargeTimeScale), adjustmentLookup);
        chargeFlowService.recoverAtStartup(std::chrono::system_clock::now());
        chargeFlowService.runMaintenance(std::chrono::system_clock::now());
        ncs::server::websocket::ChargeProgressPusher progressPusher(chargeFlowService, hub);
        ncs::core::application::AdminAuthService adminAuthService(repository, sessions);
        ncs::core::application::AdminAccountService adminAccountService(repository, sessions);
        ncs::core::application::AdminUserService adminUserService(repository, chargeFlowService,
                                                                  sessions);
        ncs::core::application::AdminStationService adminStationService(
            repository, repository, chargeFlowService, businessNumbers);
        ncs::core::application::DashboardService dashboardService(repository, repository,
                                                                  repository, repository);
        ncs::infrastructure::files::FileModelArtifactStore modelArtifacts(
            startup.config.mlModelPath);
        ncs::core::application::MlService mlService(repository, repository, repository,
                                                    businessNumbers, &modelArtifacts);
        ncs::server::runtime::MlProcessManager mlProcessManager(
            ncs::server::runtime::MlProcessOptions{
                startup.config.pythonExecutable, startup.config.mlWorkerScript,
                localServerUrl(startup.config), startup.config.tlsCertificatePath,
                startup.config.mlModelPath},
            sessions, repository, repository, &modelArtifacts);
        ncs::core::application::AdminOpsService adminOpsService(repository, repository,
                                                                chargeFlowService, businessNumbers,
                                                                &repository, &mlProcessManager);
        // Declared after every service captured by submitted work, so it is
        // also destroyed first during exception unwinding.
        ncs::core::application::BoundedExecutor blockingExecutor(
            startup.config.blockingWorkerThreads, startup.config.blockingQueueCapacity);
        // Revocation may fire on a crow io thread. Handing the notification to
        // the blocking pool keeps frame-then-close ordering on the wire (the hub
        // sends both sequentially outside its lock).
        sessions.setRevocationObserver(
            [hub, &blockingExecutor](const std::int64_t sessionId,
                                     const std::string_view principalId)
            {
                if (!blockingExecutor.submit(
                        [hub, sessionId, principalId = std::string(principalId)]
                        { hub->notifySessionRevoked(sessionId, principalId); }))
                {
                    // Queue full or shutting down: close without the revoked frame
                    // instead of leaving the revoked session connected. (Closing
                    // directly is safe: the hub closes peers outside its lock.)
                    hub->closeSession(sessionId, 4001, "session revoked");
                }
            });
        ncs::server::controller::WalletRoutes walletRoutes(apiRoutes, walletService, sessions,
                                                           blockingExecutor, idempotency);
        ncs::server::controller::StationRoutes stationRoutes(apiRoutes, stationService, sessions,
                                                             blockingExecutor);
        ncs::server::controller::NavigationRoutes navigationRoutes(apiRoutes, navigationService,
                                                                   sessions, blockingExecutor);
        ncs::server::controller::FlowRoutes flowRoutes(apiRoutes, chargeFlowService, sessions,
                                                       blockingExecutor, idempotency);
        ncs::server::controller::UserIdentityRoutes userIdentityRoutes(
            apiRoutes, userIdentity, sessions, blockingExecutor,
            startup.config.demoCredentialsEnabled());
        ncs::server::controller::AdminRoutes adminRoutes(
            apiRoutes, adminAuthService, adminUserService, adminStationService, adminOpsService,
            sessions, blockingExecutor, idempotency, adminAccountService);
        ncs::server::controller::DashboardRoutes dashboardRoutes(
            apiRoutes, adminAuthService, dashboardService, sessions, blockingExecutor,
            startup.config.dashboardSnapshotPath, hub);
        ncs::server::controller::MlRoutes mlRoutes(apiRoutes, mlService, sessions, blockingExecutor,
                                                   idempotency);
        if (!dashboardRoutes.refreshAndExport(std::chrono::system_clock::now()))
        {
            logger->log(LogLevel::Warning, "dashboard.snapshot",
                        "initial dashboard snapshot unavailable");
        }
        ncs::server::websocket::WebsocketRoutes websocketRoutes(
            app, sessions, hub, startup.config.websocketMaxPayloadBytes);

        // Crow keeps a single tick slot; a multi-cadence scheduler drives every
        // periodic task (fixes the previous second tick() silently overwriting
        // the 15-second maintenance tick).
        ncs::server::runtime::PeriodicScheduler scheduler;
        // The heartbeat check runs every five seconds so the 30-second ping and
        // 60-second timeout from contract 13.4 are observed within one tick,
        // not up to a full 30-second tick late.
        scheduler.add(std::chrono::seconds(5),
                      [&hub](const auto done)
                      {
                          hub->tickHeartbeat(
                              std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count());
                          done();
                      });
        scheduler.add(std::chrono::seconds(1),
                      [&](const auto done)
                      {
                          if (!blockingExecutor.submit(
                                  [&, done]
                                  {
                                      try
                                      {
                                          progressPusher.pushOnce(std::chrono::system_clock::now());
                                      }
                                      catch (...)
                                      {
                                          // done() must always run or this entry stalls forever.
                                      }
                                      done();
                                  }))
                          {
                              done();
                          }
                      });
        scheduler.add(std::chrono::seconds(2),
                      [&](const auto done)
                      {
                          if (!blockingExecutor.submit(
                                  [&, done]
                                  {
                                      try
                                      {
                                          outboxDispatcher.dispatchOnce(
                                              std::chrono::system_clock::now());
                                      }
                                      catch (...)
                                      {
                                      }
                                      done();
                                  }))
                          {
                              done();
                          }
                      });
        scheduler.add(std::chrono::seconds(15),
                      [&](const auto done)
                      {
                          if (!blockingExecutor.submit(
                                  [&, done]
                                  {
                                      try
                                      {
                                          chargeFlowService.runMaintenance(
                                              std::chrono::system_clock::now());
                                          adminStationService.completeDueCommands(
                                              std::chrono::system_clock::now());
                                          adminOpsService.completeTimedOutMlTasks(
                                              std::chrono::system_clock::now());
                                      }
                                      catch (...)
                                      {
                                          // A transient SQLite failure must not permanently stall
                                          // the maintenance pass; the next tick retries.
                                      }
                                      done();
                                  }))
                          {
                              done();
                          }
                      });
        scheduler.add(std::chrono::seconds(30),
                      [&](const auto done)
                      {
                          if (!blockingExecutor.submit(
                                  [&, done]
                                  {
                                      try
                                      {
                                          dashboardRoutes.refreshAndExport(
                                              std::chrono::system_clock::now());
                                      }
                                      catch (...)
                                      {
                                          dashboardService.markStale();
                                      }
                                      done();
                                  }))
                          {
                              done();
                          }
                      });
        scheduler.add(
            std::chrono::seconds(60),
            [&](const auto done)
            {
                if (!blockingExecutor.submit(
                        [&, done]
                        {
                            try
                            {
                                const auto systemNow = std::chrono::system_clock::now();
                                const auto steadyNow = std::chrono::steady_clock::now();
                                sessions.cleanup(systemNow);
                                verificationCodes.cleanup(systemNow);
                                idempotency.cleanup(systemNow);
                                repository.cleanupAdminRecords(
                                    std::chrono::duration_cast<std::chrono::seconds>(
                                        systemNow.time_since_epoch())
                                        .count());
                                repository.cleanupAnalytics(
                                    std::chrono::duration_cast<std::chrono::seconds>(
                                        systemNow.time_since_epoch())
                                        .count());
                                try
                                {
                                    // Keep the newest qualified model's artifact: PREDICT
                                    // launches re-verify it, and losing it would silently
                                    // degrade future runs to BASELINE.
                                    const auto latestModel = repository.latestQualifiedModel();
                                    modelArtifacts.cleanupExpired(
                                        latestModel
                                            ? std::optional<std::string>{latestModel->artifactPath}
                                            : std::nullopt);
                                }
                                catch (...)
                                {
                                    modelArtifacts.cleanupExpired();
                                }
                                repository.refreshReadiness();
                                requestPolicy.rateLimiter.cleanup(steadyNow);
                                requestPolicy.passwordRateLimiter.cleanup(steadyNow);
                                logger->cleanupExpired();
                            }
                            catch (...)
                            {
                                // As above: never leave done() uncalled.
                            }
                            done();
                        }))
                {
                    done();
                }
            });
        app.tick(std::chrono::seconds(1),
                 [&scheduler] { scheduler.tick(std::chrono::steady_clock::now()); });
        app.signal_clear()
            .signal_add(SIGINT)
            .signal_add(SIGTERM)
            .bindaddr(startup.config.listenAddress)
            .port(startup.config.port)
            .concurrency(startup.config.workerThreads)
            .timeout(30)
            .server_name("");
        if (startup.config.allowInsecureHttp)
        {
            app.run();
        }
        else
        {
            app.ssl(ncs::server::runtime::createTlsContext(startup.config)).run();
        }
        // Drain all accepted work while every captured dependency and Crow's
        // response storage are still alive.
        blockingExecutor.shutdown();
        // MlProcessManager is destroyed after the executor. Detach this callback
        // first so worker-token revocation during process shutdown cannot capture
        // an executor whose lifetime has ended.
        sessions.setRevocationObserver({});
        hub->shutdown();
        logger->log(LogLevel::Info, "server.lifecycle", "server stopped");
    }
    catch (const ncs::server::runtime::ConfigError& error)
    {
        logFailureSafely(logger.get(), error.what());
        std::cerr << "ncs_server configuration error: " << error.what()
                  << "\nUse --help for supported options.\n";
        return 2;
    }
    catch (const std::exception&)
    {
        logFailureSafely(logger.get(), "server startup failed");
        std::cerr << "ncs_server failed to start; inspect protected logs\n";
        return 1;
    }

    return 0;
}
