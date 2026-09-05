#include "user_main_window.h"
#include "user_demo_service.h"
#include "net/api_client.h"
#include "net/user_api.h"
#include "ui/app_theme.h"

#include "config/application_config.h"
#include "logging/application_logger.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDebug>
#include <QFile>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QTimer>
#include <QUrl>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    ncs::user::AppTheme::apply(app);
    QCoreApplication::setOrganizationName(QStringLiteral("NCS"));
    QCoreApplication::setApplicationName(QStringLiteral("ncs_user"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("NCS Qt Widgets 用户端"));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("smoke-test"), QStringLiteral("启动后自动退出")});
    parser.addOption({QStringLiteral("mock-scenario"),
                      QStringLiteral("选择 Mock 场景：happy-path、low-balance、no-available-charger、active-charging"),
                      QStringLiteral("name")});
    parser.addOption({QStringLiteral("api-request-code"),
                      QStringLiteral("向配置的 REST 服务请求一次验证码后退出"),
                      QStringLiteral("phone")});
    parser.addOption(
        {QStringLiteral("config"), QStringLiteral("指定 .env 配置文件"), QStringLiteral("path")});
    parser.process(app);

    auto config =
        ncs::infrastructure::ApplicationConfig::load(parser.value(QStringLiteral("config")));
    if (!config)
    {
        qCritical().noquote() << config.error().userMessage;
        return 2;
    }
    auto logger = ncs::infrastructure::ApplicationLogger::initialize(config.value().logDirectory(),
                                                                     QStringLiteral("user"));
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

    if (parser.isSet(QStringLiteral("api-request-code")))
    {
        QUrl baseUrl;
        baseUrl.setScheme(config.value().allowInsecureHttp() ? QStringLiteral("http") : QStringLiteral("https"));
        baseUrl.setHost(config.value().serverHost());
        baseUrl.setPort(config.value().serverPort());
        auto* apiClient = new ncs::user::ApiClient(baseUrl, &app);
        auto* userApi = new ncs::user::UserApi(*apiClient);
        userApi->requestSmsCode(parser.value(QStringLiteral("api-request-code")),
                                 [&app](ncs::user::ApiReply reply) {
            qInfo().noquote() << (reply.ok() ? QStringLiteral("REST 验证码请求成功")
                                              : QStringLiteral("REST 请求失败：") + reply.message);
            app.exit(reply.ok() ? 0 : 4);
        });
        const int result = app.exec();
        ncs::infrastructure::ApplicationLogger::shutdown();
        return result;
    }

    QUrl baseUrl;
    baseUrl.setScheme(config.value().allowInsecureHttp() ? QStringLiteral("http") : QStringLiteral("https"));
    baseUrl.setHost(config.value().serverHost());
    baseUrl.setPort(config.value().serverPort());
    ncs::user::ApiClient apiClient(baseUrl, &app);
    ncs::user::UserApi userApi(apiClient);
    ncs::user::MockUserClientService mockService;
    const bool mockMode = parser.isSet(QStringLiteral("mock-scenario"));
    QString scenarioMessage;
    if (mockMode && !mockService.configureScenario(parser.value(QStringLiteral("mock-scenario")), &scenarioMessage))
    {
        qCritical().noquote() << scenarioMessage;
        ncs::infrastructure::ApplicationLogger::shutdown();
        return 4;
    }
    if (mockMode) qInfo().noquote() << scenarioMessage;
    ncs::user::UserMainWindow window(mockService, mockMode ? nullptr : &apiClient,
                                     mockMode ? nullptr : &userApi);
    window.show();
    if (parser.isSet(QStringLiteral("smoke-test")))
    {
        QTimer::singleShot(150, &app, &QCoreApplication::quit);
    }

    const int result = app.exec();
    ncs::infrastructure::ApplicationLogger::shutdown();
    return result;
}
