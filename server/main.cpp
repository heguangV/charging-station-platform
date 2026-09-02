#include "config/application_config.h"
#include "logging/application_logger.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTimer>

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("NCS"));
    QCoreApplication::setApplicationName(QStringLiteral("ncs_server"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("NCS 业务服务端基础骨架"));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("smoke-test"), QStringLiteral("完成初始化后自动退出")});
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
                                                                     QStringLiteral("server"));
    if (!logger)
    {
        qCritical().noquote() << logger.error().userMessage;
        return 3;
    }

    qInfo().noquote() << QStringLiteral("NCS server foundation initialized:")
                      << config.value().safeSummary().join(QStringLiteral(", "));
    qInfo() << "REST/WebSocket transport is scheduled for delivery stage 3.";

    if (parser.isSet(QStringLiteral("smoke-test")))
    {
        QTimer::singleShot(0, &app, &QCoreApplication::quit);
    }

    const int result = app.exec();
    ncs::infrastructure::ApplicationLogger::shutdown();
    return result;
}
