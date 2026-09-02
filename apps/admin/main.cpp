#include "admin_main_window.h"

#include "config/application_config.h"
#include "logging/application_logger.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QTimer>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("NCS"));
    QCoreApplication::setApplicationName(QStringLiteral("ncs_admin"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("NCS Qt Widgets 管理端"));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("smoke-test"), QStringLiteral("启动后自动退出")});
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
                                                                     QStringLiteral("admin"));
    if (!logger)
    {
        qCritical().noquote() << logger.error().userMessage;
        return 3;
    }

    ncs::admin::AdminMainWindow window;
    window.show();
    if (parser.isSet(QStringLiteral("smoke-test")))
    {
        QTimer::singleShot(150, &app, &QCoreApplication::quit);
    }

    const int result = app.exec();
    ncs::infrastructure::ApplicationLogger::shutdown();
    return result;
}
