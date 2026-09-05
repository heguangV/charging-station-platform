#include "user_main_window.h"

#include "net/user_api.h"

#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtMath>

namespace ncs::user
{

QWidget* UserMainWindow::createProfilePage()
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 8, 0, 0);
    layout->setSpacing(12);
    auto* title = new QLabel(QStringLiteral("我的账户"));
    title->setStyleSheet(QStringLiteral("font-size:23px;color:#25324A;"));
    layout->addWidget(title);

    auto* info = new QFrame;
    info->setObjectName(QStringLiteral("card"));
    auto* form = new QFormLayout(info);
    form->setContentsMargins(18, 18, 18, 18);
    profileAvatar_ = new QLabel(QStringLiteral("NCS"));
    profileAvatar_->setFixedSize(68, 68);
    profileAvatar_->setAlignment(Qt::AlignCenter);
    profileAvatar_->setStyleSheet(
        QStringLiteral("background:#D7F0EB;color:#0F766E;border-radius:34px;font-weight:700;"));
    auto* avatarRow = new QHBoxLayout;
    avatarRow->addWidget(profileAvatar_);
    auto* avatar = button(QStringLiteral("更换头像"));
    avatar->setMinimumHeight(34);
    avatarRow->addWidget(avatar);
    avatarRow->addStretch();
    nicknameEdit_ = new QLineEdit;
    nicknameEdit_->setMaxLength(20);
    profileName_ = new QLabel;
    profileBalance_ = new QLabel;
    profileBalance_->setStyleSheet(QStringLiteral("font-size:24px;font-weight:700;color:#147A50;"));
    form->addRow(QStringLiteral("头像"), avatarRow);
    form->addRow(QStringLiteral("昵称"), nicknameEdit_);
    form->addRow(QStringLiteral("手机号"), profileName_);
    form->addRow(QStringLiteral("余额"), profileBalance_);
    layout->addWidget(info);

    auto* save = button(QStringLiteral("保存昵称"));
    auto* recharge = button(QStringLiteral("余额充值"));
    auto* orders = button(QStringLiteral("我的订单"));
    auto* home = button(QStringLiteral("返回首页"),
                        QStringLiteral("QPushButton{background:#E2F3F0;color:#0F766E;border:0;"
                                       "border-radius:10px;font-size:15px;font-weight:600;}"));
    auto* logout = button(QStringLiteral("退出登录"),
                          QStringLiteral("QPushButton{background:#FFF0F0;color:#B42318;border:0;"
                                         "border-radius:10px;font-size:15px;font-weight:600;}"));
    layout->addWidget(save);
    layout->addWidget(recharge);
    layout->addWidget(orders);
    layout->addWidget(home);
    layout->addWidget(logout);
    layout->addStretch();
    connect(avatar, &QPushButton::clicked, this,
            [this]
            {
                const QString file =
                    QFileDialog::getOpenFileName(this, QStringLiteral("选择头像"), {},
                                                 QStringLiteral("图片 (*.png *.jpg *.jpeg *.bmp)"));
                if (!file.isEmpty())
                {
                    QString message;
                    service_.updateAvatar(file, &message);
                    refreshProfile();
                    notify(message);
                }
            });
    connect(save, &QPushButton::clicked, this,
            [this]
            {
                QString message;
                if (service_.updateNickname(nicknameEdit_->text(), &message))
                    refreshProfile();
                notify(message, message != QStringLiteral("昵称已保存"));
            });
    connect(recharge, &QPushButton::clicked, this,
            [this]
            {
                bool ok = false;
                const double value = QInputDialog::getDouble(this, QStringLiteral("余额充值"),
                                                             QStringLiteral("金额（元）"), 100.0,
                                                             0.01, 10000.0, 2, &ok);
                if (ok)
                {
                    QString message;
                    service_.recharge(qRound(value * 100), &message);
                    refreshProfile();
                    notify(message);
                }
            });
    connect(home, &QPushButton::clicked, this, &UserMainWindow::showHome);
    connect(orders, &QPushButton::clicked, this, &UserMainWindow::showOrders);
    connect(logout, &QPushButton::clicked, this,
            [this]
            {
                QString message;
                service_.logout(&message);
                if (onlineSession_ && userApi_)
                {
                    userApi_->logout([this](ApiReply) { userApi_->setAccessToken({}); });
                }
                onlineSession_ = false;
                ++navigationRequestId_;
                notify(message);
                showLogin();
            });
    return page;
}

} // namespace ncs::user
