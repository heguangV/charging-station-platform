#include "mainwindow.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDebug>
#include <QTimer>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Codex Qt Demo"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("一个用于学习 Qt Widgets 的小项目"));
    parser.addHelpOption();

    const QCommandLineOption smokeTestOption(
        QStringLiteral("smoke-test"),
        QStringLiteral("启动窗口后自动退出，用于无桌面环境的运行检查。"));
    const QCommandLineOption screenshotOption(
        QStringLiteral("screenshot"),
        QStringLiteral("启动后把窗口截图保存到指定路径并退出。"),
        QStringLiteral("path"));
    parser.addOption(smokeTestOption);
    parser.addOption(screenshotOption);
    parser.process(app);

    MainWindow window;
    window.show();

    if (parser.isSet(smokeTestOption) || parser.isSet(screenshotOption)) {
        const QString screenshotPath = parser.value(screenshotOption);
        QTimer::singleShot(300, &app, [&app, &window, screenshotPath] {
            if (!screenshotPath.isEmpty()) {
                const bool saved = window.grab().save(screenshotPath);
                qInfo() << (saved ? "Screenshot saved:" : "Screenshot failed:")
                        << screenshotPath;
            }
            qInfo() << "Qt demo smoke test passed.";
            app.quit();
        });
    }

    return app.exec();
}
