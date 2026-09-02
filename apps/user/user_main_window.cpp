#include "user_main_window.h"

#include <QLabel>

namespace ncs::user
{

UserMainWindow::UserMainWindow(QWidget* parent) : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("NCS 充电 - 用户端"));
    setFixedSize(420, 760);

    auto* message = new QLabel(
        QStringLiteral("NCS 用户端\n\n工程基础骨架已就绪\n业务页面将在用户端阶段实现"), this);
    message->setAlignment(Qt::AlignCenter);
    message->setWordWrap(true);
    setCentralWidget(message);
}

} // namespace ncs::user
