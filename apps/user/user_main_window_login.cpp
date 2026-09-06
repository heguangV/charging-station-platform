#include "user_main_window.h"

#include "net/api_client.h"
#include "net/user_api.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

namespace ncs::user
{
QWidget* UserMainWindow::createLoginPage()
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(16, 34, 16, 16);
    layout->setSpacing(14);
    auto* brand = new QLabel(QStringLiteral("NCS"));
    brand->setFixedSize(58, 58);
    brand->setAlignment(Qt::AlignCenter);
    brand->setStyleSheet(QStringLiteral("background:#167C55;color:white;border-radius:16px;"
                                        "font-size:23px;font-weight:800;"));
    layout->addWidget(brand);
    auto* brandName = new QLabel(QStringLiteral("NCS 充电"));
    brandName->setStyleSheet(QStringLiteral("color:#167C55;font-size:13px;font-weight:700;"));
    layout->addWidget(brandName);
    layout->addSpacing(18);
    auto* title = new QLabel(QStringLiteral("嗨，欢迎回来"));
    title->setStyleSheet(QStringLiteral("color:#151A21;font-size:32px;font-weight:800;"));
    layout->addWidget(title);
    auto* subtitle = new QLabel(QStringLiteral("为下一程充好电，轻松出发"));
    subtitle->setStyleSheet(QStringLiteral("color:#555D67;font-size:16px;"));
    layout->addWidget(subtitle);
    layout->addSpacing(32);
    auto* phoneRow = new QFrame;
    phoneRow->setStyleSheet(
        QStringLiteral("QFrame{background:#F5F6F8;border:0;border-radius:12px;}"));
    auto* phoneLayout = new QHBoxLayout(phoneRow);
    phoneLayout->setContentsMargins(16, 3, 12, 3);
    auto* prefix = new QLabel(QStringLiteral("+86  |"));
    prefix->setStyleSheet(QStringLiteral("color:#737A83;font-size:18px;"));
    phoneLayout->addWidget(prefix);
    phoneEdit_ = new QLineEdit;
    phoneEdit_->setPlaceholderText(QStringLiteral("请输入 11 位手机号"));
    phoneEdit_->setMaxLength(11);
    phoneEdit_->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("1\\d{0,10}")), phoneEdit_));
    phoneEdit_->setInputMethodHints(Qt::ImhDigitsOnly);
    phoneEdit_->setStyleSheet(QStringLiteral("QLineEdit{background:transparent;border:0;"
                                             "font-size:18px;font-weight:600;padding:12px 0;}"));
    phoneLayout->addWidget(phoneEdit_, 1);
    layout->addWidget(phoneRow);
    auto* codeRow = new QHBoxLayout;
    codeRow->setSpacing(10);
    codeEdit_ = new QLineEdit;
    codeEdit_->setPlaceholderText(QStringLiteral("输入验证码"));
    codeEdit_->setMaxLength(6);
    codeEdit_->setMinimumHeight(50);
    codeButton_ = button(QStringLiteral("获取验证码"));
    codeButton_->setMinimumSize(118, 50);
    codeButton_->setObjectName(QStringLiteral("secondaryButton"));
    codeRow->addWidget(codeEdit_, 1);
    codeRow->addWidget(codeButton_);
    layout->addLayout(codeRow);
    auto* login = button(QStringLiteral("登录"));
    login->setMinimumHeight(54);
    layout->addSpacing(4);
    layout->addWidget(login);
    auto* demoHelp = new QToolButton;
    demoHelp->setText(QStringLiteral("ⓘ 演示说明"));
    demoHelp->setToolTip(userApi_ ? QStringLiteral("开发环境会在获取验证码后显示本次验证码。")
                                  : QStringLiteral("演示验证码为 123456。"));
    demoHelp->setCursor(Qt::PointingHandCursor);
    demoHelp->setStyleSheet(QStringLiteral("QToolButton{color:#737A83;border:0;font-size:13px;"
                                           "padding:6px 0;background:transparent;}"));
    layout->addWidget(demoHelp, 0, Qt::AlignRight);
    layout->addStretch();
    auto* agreement = new QLabel(QStringLiteral("登录即表示你同意 NCS 服务条款与隐私说明"));
    agreement->setWordWrap(true);
    agreement->setAlignment(Qt::AlignCenter);
    agreement->setStyleSheet(QStringLiteral("font-size:12px;color:#757D87;"));
    layout->addWidget(agreement);
    connect(
        codeButton_, &QPushButton::clicked, this,
        [this]
        {
            if (phoneEdit_->text().size() != 11 || !phoneEdit_->text().startsWith(QLatin1Char('1')))
            {
                notify(QStringLiteral("请输入正确的 11 位手机号"), true);
                return;
            }
            codeButton_->setEnabled(false);
            if (!userApi_)
            {
                codeCountdown_ = 60;
                notify(QStringLiteral("验证码已发送，请输入 %1").arg(service_.developmentCode()));
                return;
            }
            userApi_->requestSmsCode(
                phoneEdit_->text(),
                [this](ApiReply reply)
                {
                    if (!reply.ok())
                    {
                        codeButton_->setEnabled(true);
                        notify(reply.message + QStringLiteral("；可在服务恢复前使用本机演示"),
                               true);
                        return;
                    }
                    codeCountdown_ = qMax(
                        1, reply.data.toObject().value(QStringLiteral("retryAfterSec")).toInt(60));
                    const QString developmentCode =
                        reply.data.toObject().value(QStringLiteral("developmentCode")).toString();
                    notify(developmentCode.isEmpty()
                               ? QStringLiteral("验证码已发送")
                               : QStringLiteral("验证码已发送，请输入 %1").arg(developmentCode));
                });
        });
    connect(demoHelp, &QToolButton::clicked, this,
            [demoHelp]
            {
                QToolTip::showText(demoHelp->mapToGlobal(QPoint(0, demoHelp->height())),
                                   demoHelp->toolTip(), demoHelp);
            });
    connect(
        login, &QPushButton::clicked, this,
        [this, login]
        {
            if (phoneEdit_->text().size() != 11)
            {
                notify(QStringLiteral("请输入正确的 11 位手机号"), true);
                return;
            }
            if (!userApi_)
            {
                QString message;
                if (!service_.login(phoneEdit_->text(), codeEdit_->text(), &message))
                {
                    notify(message, true);
                    return;
                }
                notify(message);
                showHome();
                return;
            }
            const QString phone = phoneEdit_->text();
            const QString smsCode = codeEdit_->text();
            login->setEnabled(false);
            userApi_->loginSms(
                phone, smsCode, QStringLiteral("ncs-user-desktop"),
                [this, login, phone, smsCode](ApiReply reply)
                {
                    login->setEnabled(true);
                    if (reply.ok())
                    {
                        const QString token =
                            reply.data.toObject().value(QStringLiteral("accessToken")).toString();
                        if (token.isEmpty())
                        {
                            notify(QStringLiteral("服务端登录响应缺少会话令牌"), true);
                            return;
                        }
                        userApi_->setAccessToken(token);
                        onlineSession_ = true;
                        notify(QStringLiteral("登录成功，腾讯地图路线服务已连接"));
                        showHome();
                        return;
                    }
                    if (reply.code != QStringLiteral("NetworkError"))
                    {
                        notify(reply.message, true);
                        return;
                    }
                    QString message;
                    if (!service_.login(phone, smsCode, &message))
                    {
                        notify(message, true);
                        return;
                    }
                    onlineSession_ = false;
                    notify(message + QStringLiteral("；服务端不可用，已进入本机降级模式"));
                    showHome();
                });
        });
    return page;
}

} // namespace ncs::user
