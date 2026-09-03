#include "user_main_window.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QPushButton>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

namespace ncs::user
{

QWidget* UserMainWindow::createLoginPage()
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(14, 30, 14, 24);
    layout->setSpacing(14);
    auto* brand = new QFrame;
    brand->setStyleSheet(QStringLiteral("QFrame{background:#0F766E;border-radius:20px;}"));
    auto* brandLayout = new QVBoxLayout(brand);
    brandLayout->setContentsMargins(22, 22, 22, 22);
    auto* eyebrow = new QLabel(QStringLiteral("NCS · 智慧充电服务"));
    eyebrow->setStyleSheet(QStringLiteral("color:#B9F3E9;font-size:12px;font-weight:600;"));
    auto* title = new QLabel(QStringLiteral("让每次出行，\n都从容满电"));
    title->setStyleSheet(QStringLiteral("color:white;font-size:27px;font-weight:700;"));
    auto* subtitle = new QLabel(QStringLiteral("查找附近好桩，预约、充电、结算一站完成"));
    subtitle->setWordWrap(true);
    subtitle->setStyleSheet(QStringLiteral("color:#D4F7F0;font-size:13px;"));
    brandLayout->addWidget(eyebrow);
    brandLayout->addSpacing(8);
    brandLayout->addWidget(title);
    brandLayout->addSpacing(8);
    brandLayout->addWidget(subtitle);
    layout->addWidget(brand);
    layout->addSpacing(8);
    auto* loginTitle = new QLabel(QStringLiteral("手机号登录"));
    loginTitle->setStyleSheet(QStringLiteral("font-size:21px;font-weight:700;color:#25324A;"));
    layout->addWidget(loginTitle);
    auto* loginHint = new QLabel(QStringLiteral("首次登录将自动创建账户"));
    loginHint->setStyleSheet(QStringLiteral("font-size:13px;color:#667085;"));
    layout->addWidget(loginHint);
    phoneEdit_ = new QLineEdit;
    phoneEdit_->setPlaceholderText(QStringLiteral("请输入 11 位手机号"));
    phoneEdit_->setMaxLength(11);
    phoneEdit_->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("1\\d{0,10}")), phoneEdit_));
    phoneEdit_->setInputMethodHints(Qt::ImhDigitsOnly);
    layout->addWidget(phoneEdit_);
    auto* codeRow = new QHBoxLayout;
    codeEdit_ = new QLineEdit;
    codeEdit_->setPlaceholderText(QStringLiteral("请输入验证码"));
    codeEdit_->setMaxLength(6);
    codeButton_ = button(QStringLiteral("获取验证码"));
    codeButton_->setMinimumWidth(120);
    codeRow->addWidget(codeEdit_);
    codeRow->addWidget(codeButton_);
    layout->addLayout(codeRow);
    auto* login = button(QStringLiteral("登录并开始找桩"));
    layout->addWidget(login);
    auto* demoHelp = new QToolButton;
    demoHelp->setText(QStringLiteral("ⓘ 演示说明"));
    demoHelp->setToolTip(QStringLiteral("演示验证码为 123456；验证码不会写入日志。"));
    demoHelp->setCursor(Qt::PointingHandCursor);
    demoHelp->setStyleSheet(QStringLiteral("QToolButton{color:#0F766E;border:0;background:transparent;font-size:12px;padding:3px 0;text-align:left;}"));
    layout->addWidget(demoHelp, 0, Qt::AlignLeft);
    layout->addStretch();
    auto* agreement = new QLabel(QStringLiteral("登录即表示你同意 NCS 服务条款与隐私说明"));
    agreement->setAlignment(Qt::AlignCenter);
    agreement->setStyleSheet(QStringLiteral("font-size:11px;color:#98A2B3;"));
    layout->addWidget(agreement);
    connect(codeButton_, &QPushButton::clicked, this, [this] {
        if (phoneEdit_->text().size() != 11 || !phoneEdit_->text().startsWith(QLatin1Char('1')))
        {
            notify(QStringLiteral("请输入正确的 11 位手机号"), true);
            return;
        }
        codeCountdown_ = 60;
        codeButton_->setEnabled(false);
        notify(QStringLiteral("验证码已发送，请输入 %1").arg(service_.developmentCode()));
    });
    connect(demoHelp, &QToolButton::clicked, this, [demoHelp] {
        QToolTip::showText(demoHelp->mapToGlobal(QPoint(0, demoHelp->height())), demoHelp->toolTip(), demoHelp);
    });
    connect(login, &QPushButton::clicked, this, [this] {
        if (phoneEdit_->text().size() != 11)
        {
            notify(QStringLiteral("请输入正确的 11 位手机号"), true);
            return;
        }
        QString message;
        if (!service_.login(phoneEdit_->text(), codeEdit_->text(), &message))
        {
            notify(message, true);
            return;
        }
        notify(message);
        showHome();
    });
    return page;
}

} // namespace ncs::user
