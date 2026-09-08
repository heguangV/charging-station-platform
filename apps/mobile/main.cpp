#include "mobile_api.h"
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QUrl>
#include <QtWebView>

int main(int argc, char* argv[])
{
    QtWebView::initialize();
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("NCS"));
    app.setApplicationName(QStringLiteral("NcsMobile"));
    ncs::mobile::MobileApi api;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("mobileApi"), &api);
    engine.load(QUrl(QStringLiteral("qrc:/qt/qml/NcsMobile/Main.qml")));
    if (engine.rootObjects().isEmpty())
        return 1;
    return app.exec();
}
