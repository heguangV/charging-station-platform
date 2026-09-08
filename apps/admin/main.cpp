#include "admin_api_client.h"
#include "admin_main_window.h"
#include <QFile>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QUrl>

#include "config/application_config.h"
#include "logging/application_logger.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QTimer>
#include <QUuid>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setStyle(QStringLiteral("Fusion"));
    QCoreApplication::setOrganizationName(QStringLiteral("NCS"));
    QCoreApplication::setApplicationName(QStringLiteral("ncs_admin"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("NCS Qt Widgets 管理端"));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("smoke-test"), QStringLiteral("启动后自动退出")});
    parser.addOption(
        {QStringLiteral("config"), QStringLiteral("指定 .env 配置文件"), QStringLiteral("path")});
    parser.addOption({QStringLiteral("api-smoke-test"),
                      QStringLiteral("使用环境变量 NCS_ADMIN_TEST_USERNAME / "
                                     "NCS_ADMIN_TEST_PASSWORD 登录、读取统计并退出")});
    parser.process(app);

    auto config =
        ncs::infrastructure::ApplicationConfig::load(parser.value(QStringLiteral("config")));
    if (!config)
    {
        qCritical().noquote() << config.error().userMessage;
        return 2;
    }
    auto logger = ncs::infrastructure::ApplicationLogger::initialize(config.value().logDirectory(),
                                                                     QStringLiteral("admin"));
    if (!logger)
    {
        qCritical().noquote() << logger.error().userMessage;
        return 3;
    }

    const QString tlsCaPath = qEnvironmentVariable("NCS_TLS_CA_PATH").trimmed();
    if (!tlsCaPath.isEmpty())
    {
        QFile caFile(tlsCaPath);
        if (!caFile.open(QIODevice::ReadOnly))
        {
            qCritical().noquote() << QStringLiteral("无法读取 TLS CA 证书");
            ncs::infrastructure::ApplicationLogger::shutdown();
            return 4;
        }
        const QList<QSslCertificate> certificates = QSslCertificate::fromData(caFile.readAll());
        if (certificates.isEmpty())
        {
            qCritical().noquote() << QStringLiteral("TLS CA 证书格式无效");
            ncs::infrastructure::ApplicationLogger::shutdown();
            return 4;
        }
        QSslConfiguration sslConfiguration = QSslConfiguration::defaultConfiguration();
        auto trustedCertificates = sslConfiguration.caCertificates();
        trustedCertificates.append(certificates);
        sslConfiguration.setCaCertificates(trustedCertificates);
        QSslConfiguration::setDefaultConfiguration(sslConfiguration);
    }

    QUrl baseUrl;
    baseUrl.setScheme(config.value().allowInsecureHttp() ? QStringLiteral("http")
                                                         : QStringLiteral("https"));
    baseUrl.setHost(config.value().serverHost());
    baseUrl.setPort(config.value().serverPort());
    baseUrl.setPath(QStringLiteral("/api/v1/"));
    ncs::admin::AdminApiClient apiClient(baseUrl, &app);
    ncs::admin::AdminMainWindow window(apiClient, config.value().environment());
    if (parser.isSet(QStringLiteral("api-smoke-test")))
    {
        const auto username = qEnvironmentVariable("NCS_ADMIN_TEST_USERNAME");
        const auto password = qEnvironmentVariable("NCS_ADMIN_TEST_PASSWORD");
        if (username.isEmpty() || password.isEmpty())
        {
            qCritical() << "Admin API probe requires test credentials in environment variables";
            ncs::infrastructure::ApplicationLogger::shutdown();
            return 5;
        }
        QTimer::singleShot(25000, &app, [&app] { app.exit(6); });
        QObject::connect(&apiClient, &ncs::admin::AdminApiClient::sessionExpired, &app,
                         [&app] { app.exit(7); });
        QTimer::singleShot(
            0, &app,
            [&apiClient, &app, username, password]
            {
                apiClient.postJson(
                    "admin/auth/login",
                    {{"username", username},
                     {"password", password},
                     {"deviceId", QUuid::createUuid().toString(QUuid::WithoutBraces)}},
                    [&apiClient, &app](ncs::admin::AdminReply reply)
                    {
                        const auto token = reply.data.toObject().value("accessToken").toString();
                        if (!reply.ok() || token.isEmpty())
                        {
                            app.exit(7);
                            return;
                        }
                        apiClient.setAccessToken(token);
                        apiClient.get(
                            "admin/stats/charger-status", {},
                            [&apiClient, &app](ncs::admin::AdminReply stats)
                            {
                                const bool valid =
                                    stats.ok() &&
                                    stats.data.toObject().value("occupiedCount").isDouble() &&
                                    stats.data.toObject().value("healthPercent").isDouble();
                                apiClient.postJson("admin/auth/logout", {},
                                                   [&app, valid](ncs::admin::AdminReply logout)
                                                   { app.exit(valid && logout.ok() ? 0 : 8); });
                            });
                    });
            });
    }
    else
        window.show();
    if (parser.isSet(QStringLiteral("smoke-test")))
    {
        QTimer::singleShot(150, &app, &QCoreApplication::quit);
    }

    const int result = app.exec();
    ncs::infrastructure::ApplicationLogger::shutdown();
    return result;
}
