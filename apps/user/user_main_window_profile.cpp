#include "user_main_window.h"

#include "net/api_client.h"
#include "net/user_api.h"

#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
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
    profileAvatar_->setStyleSheet(QStringLiteral("background:#D7F0EB;color:#0F766E;border-radius:34px;font-weight:700;"));
    auto* avatarRow = new QHBoxLayout;
    avatarRow->addWidget(profileAvatar_);
    auto* avatar = button(QStringLiteral("更换头像"));
    avatar->setObjectName(QStringLiteral("secondaryButton"));
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
    save->setObjectName(QStringLiteral("secondaryButton"));
    auto* recharge = button(QStringLiteral("余额充值"));
    layout->addWidget(recharge);
    layout->addWidget(save);
    layout->addSpacing(2);
    auto* logout = button(QStringLiteral("退出登录"));
    logout->setObjectName(QStringLiteral("dangerButton"));
    auto* deleteAccount = button(QStringLiteral("申请注销账户"));
    deleteAccount->setObjectName(QStringLiteral("dangerButton"));
    layout->addWidget(logout);
    layout->addWidget(deleteAccount);
    layout->addStretch();
    connect(avatar, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("选择头像"), {},
                                                           QStringLiteral("图片 (*.png *.jpg *.jpeg *.bmp)"));
        if (file.isEmpty()) return;
        if (QFileInfo(file).size() > 5 * 1024 * 1024)
        {
            notify(QStringLiteral("头像文件不能超过 5 MiB"), true);
            return;
        }
        if (!userApi_)
        {
            QString message;
            service_.updateAvatar(file, &message);
            refreshProfile();
            notify(message);
            return;
        }
        auto* image = new QFile(file);
        if (!image->open(QIODevice::ReadOnly))
        {
            delete image;
            notify(QStringLiteral("无法读取所选头像"), true);
            return;
        }
        userApi_->uploadAvatar(image, QFileInfo(file).fileName(), [this](ApiReply reply) {
            if (!reply.ok())
            {
                notify(reply.message, true);
                return;
            }
            refreshProfile();
            notify(QStringLiteral("头像已更新"));
        });
    });
    connect(save, &QPushButton::clicked, this, [this] {
        if (nicknameEdit_->text().trimmed().isEmpty())
        {
            notify(QStringLiteral("昵称不能为空"), true);
            return;
        }
        if (userApi_)
        {
            userApi_->updateProfile(nicknameEdit_->text().trimmed(), profileVersion_, [this](ApiReply reply) {
                if (!reply.ok())
                {
                    notify(reply.message, true);
                    return;
                }
                refreshProfile();
                notify(QStringLiteral("昵称已保存"));
            });
            return;
        }
        QString message;
        if (service_.updateNickname(nicknameEdit_->text(), &message)) refreshProfile();
        notify(message, message != QStringLiteral("昵称已保存"));
    });
    connect(recharge, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const double value = QInputDialog::getDouble(this, QStringLiteral("余额充值"), QStringLiteral("金额（元）"), 100.0, 0.01, 10000.0, 2, &ok);
        if (!ok) return;
        if (userApi_)
        {
            userApi_->recharge(qRound(value * 100), [this](ApiReply reply) {
                if (!reply.ok())
                {
                    notify(reply.message, true);
                    return;
                }
                refreshProfile();
                notify(QStringLiteral("余额充值成功"));
            });
            return;
        }
        QString message;
        service_.recharge(qRound(value * 100), &message);
        refreshProfile();
        notify(message);
    });
    connect(logout, &QPushButton::clicked, this, [this] {
        if (userApi_)
        {
            userApi_->logout([this](ApiReply reply) {
                apiClient_->setAccessToken({});
                notify(reply.ok() ? QStringLiteral("已退出登录") : reply.message, !reply.ok());
                showLogin();
            });
            return;
        }
        QString message;
        service_.logout(&message);
        notify(message);
        showLogin();
    });
    connect(deleteAccount, &QPushButton::clicked, this, [this] {
        if (!userApi_)
        {
            notify(QStringLiteral("演示模式不支持注销账户"), true);
            return;
        }
        if (QMessageBox::warning(this, QStringLiteral("确认注销账户"),
                                 QStringLiteral("注销后将撤销全部登录会话并匿名化账户信息。此操作不可撤销，是否继续？"),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
        userApi_->requestPasswordResetCode(phoneEdit_->text(), [this](ApiReply codeReply) {
            if (!codeReply.ok())
            {
                notify(codeReply.message, true);
                return;
            }
            const QString developmentCode = codeReply.data.toObject().value(QStringLiteral("developmentCode")).toString();
            notify(developmentCode.isEmpty() ? QStringLiteral("注销验证码已发送")
                                              : QStringLiteral("开发验证码：%1").arg(developmentCode));
            bool ok = false;
            const QString smsCode = QInputDialog::getText(this, QStringLiteral("验证注销"),
                                                           QStringLiteral("请输入注销验证码"), QLineEdit::Normal,
                                                           {}, &ok).trimmed();
            if (!ok || smsCode.isEmpty()) return;
            userApi_->deleteAccount(smsCode, [this](ApiReply deleteReply) {
                if (!deleteReply.ok())
                {
                    notify(deleteReply.message, true);
                    return;
                }
                apiClient_->setAccessToken({});
                notify(QStringLiteral("账户已注销"));
                showLogin();
            });
        });
    });
    return page;
}

} // namespace ncs::user
