#include "admin_main_window.h"

#include <QLabel>
#include <QStatusBar>

namespace ncs::admin
{

AdminMainWindow::AdminMainWindow(QWidget* parent) : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("NCS 充电 - 管理端"));
    setMinimumSize(1280, 800);
    resize(1280, 800);

    auto* message = new QLabel(
        QStringLiteral("NCS 管理端\n\n工程基础骨架已就绪\n业务页面将在管理端阶段实现"), this);
    message->setAlignment(Qt::AlignCenter);
    message->setWordWrap(true);
    setCentralWidget(message);
    statusBar()->showMessage(QStringLiteral("服务连接：尚未启用"));
}

} // namespace ncs::admin
