#include "mapconfiguration.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTimer>
#include <QtWebView/qtwebviewfunctions.h>

int main(int argc, char *argv[]) {
  QtWebView::initialize();
  QGuiApplication app(argc, argv);
  QGuiApplication::setApplicationName(QStringLiteral("NCS 充电"));
  QGuiApplication::setOrganizationName(QStringLiteral("heguangV"));

  QCommandLineParser parser;
  parser.setApplicationDescription(
      QStringLiteral("NCS 充电桩 Android/Linux 用户端"));
  parser.addHelpOption();
  const QCommandLineOption smokeTestOption(
      QStringLiteral("smoke-test"),
      QStringLiteral("验证 QML 可以加载后自动退出，不访问地图网络。"));
  parser.addOption(smokeTestOption);
  parser.process(app);

  const bool smokeTest = parser.isSet(smokeTestOption);
  MapConfiguration mapConfiguration;

  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty(QStringLiteral("mapConfiguration"),
                                           &mapConfiguration);
  engine.rootContext()->setContextProperty(QStringLiteral("ncsSmokeTest"),
                                           smokeTest);

  QObject::connect(
      &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
      [] { QCoreApplication::exit(EXIT_FAILURE); }, Qt::QueuedConnection);

  engine.loadFromModule(QStringLiteral("Ncs.Mobile"), QStringLiteral("Main"));

  if (smokeTest) {
    QTimer::singleShot(300, &app, &QCoreApplication::quit);
  }

  return app.exec();
}
