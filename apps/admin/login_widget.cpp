#include "login_widget.h"
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
namespace ncs::admin
{
LoginWidget::LoginWidget(QWidget* parent) : QWidget(parent)
{
    setObjectName("loginPage");
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(56, 40, 56, 40);
    layout->setSpacing(56);
    auto* visual = new QWidget;
    auto* vl = new QVBoxLayout(visual);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->addStretch();
    auto* brand = new QLabel(QStringLiteral("NCS  /  充电运营中心"));
    brand->setObjectName("loginBrand");
    vl->addWidget(brand);
    auto* title = new QLabel(QStringLiteral("站点、设备与服务\n在这里，有序运行"));
    title->setObjectName("loginHeroTitle");
    vl->addWidget(title);
    auto* scene = new QLabel;
    scene->setPixmap(QPixmap(":/admin/charging-scene.png")
                         .scaled(540, 340, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    scene->setAlignment(Qt::AlignCenter);
    scene->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    vl->addWidget(scene);
    vl->addStretch();
    auto* panel = new QFrame;
    panel->setObjectName("loginPanel");
    panel->setMinimumWidth(360);
    panel->setMaximumWidth(430);
    auto* form = new QVBoxLayout(panel);
    form->setContentsMargins(32, 36, 32, 36);
    form->setSpacing(12);
    auto* mark = new QLabel(QStringLiteral("NCS"));
    mark->setObjectName("brandMark");
    form->addWidget(mark, 0, Qt::AlignLeft);
    form->addSpacing(12);
    auto* welcome = new QLabel(QStringLiteral("嗨，欢迎回来"));
    welcome->setObjectName("loginTitle");
    form->addWidget(welcome);
    auto* hint = new QLabel(QStringLiteral("使用管理员账号登录运营中心"));
    hint->setObjectName("muted");
    form->addWidget(hint);
    form->addSpacing(22);
    usernameEdit_ = new QLineEdit;
    usernameEdit_->setObjectName("adminUsername");
    usernameEdit_->setMaxLength(64);
    usernameEdit_->setPlaceholderText(QStringLiteral("管理员账号"));
    passwordEdit_ = new QLineEdit;
    passwordEdit_->setObjectName("adminPassword");
    passwordEdit_->setMaxLength(128);
    passwordEdit_->setEchoMode(QLineEdit::Password);
    passwordEdit_->setPlaceholderText(QStringLiteral("登录密码"));
    form->addWidget(new QLabel(QStringLiteral("账号")));
    form->addWidget(usernameEdit_);
    form->addSpacing(6);
    form->addWidget(new QLabel(QStringLiteral("密码")));
    form->addWidget(passwordEdit_);
    errorLabel_ = new QLabel;
    errorLabel_->setTextFormat(Qt::PlainText);
    errorLabel_->setWordWrap(true);
    errorLabel_->setObjectName("loginError");
    form->addWidget(errorLabel_);
    loginButton_ = new QPushButton(QStringLiteral("登录运营中心"));
    loginButton_->setObjectName("adminLoginButton");
    loginButton_->setMinimumHeight(48);
    form->addWidget(loginButton_);
    form->addSpacing(12);
    auto* note = new QLabel(QStringLiteral("账号权限由平台管理员分配"));
    note->setObjectName("muted");
    form->addWidget(note);
    layout->addWidget(visual, 1);
    layout->addWidget(panel, 0, Qt::AlignVCenter);
    lockTimer_ = new QTimer(this);
    lockTimer_->setInterval(1000);
    connect(lockTimer_, &QTimer::timeout, this,
            [this]
            {
                if (--lockRemaining_ <= 0)
                {
                    lockTimer_->stop();
                    setBusy(false);
                }
                else
                    loginButton_->setText(QStringLiteral("%1 秒后重试").arg(lockRemaining_));
            });
    connect(loginButton_, &QPushButton::clicked, this,
            [this]
            {
                if (usernameEdit_->text().trimmed().isEmpty() || passwordEdit_->text().isEmpty())
                {
                    showError(QStringLiteral("请输入账号和密码"));
                    return;
                }
                emit loginRequested(usernameEdit_->text().trimmed(), passwordEdit_->text(),
                                    QStringLiteral("admin-") +
                                        QUuid::createUuid().toString(QUuid::WithoutBraces));
            });
    connect(passwordEdit_, &QLineEdit::returnPressed, loginButton_, &QPushButton::click);
}
void LoginWidget::showError(const QString& message)
{
    errorLabel_->setText(message);
    errorLabel_->setVisible(!message.isEmpty());
}
void LoginWidget::setBusy(bool busy)
{
    loginButton_->setEnabled(!busy && lockRemaining_ <= 0);
    usernameEdit_->setEnabled(!busy);
    passwordEdit_->setEnabled(!busy);
    loginButton_->setText(busy ? QStringLiteral("正在登录…") : QStringLiteral("登录运营中心"));
}
void LoginWidget::clearPassword()
{
    passwordEdit_->clear();
}
void LoginWidget::lockForThirtySeconds()
{
    lockRemaining_ = 30;
    setBusy(false);
    loginButton_->setEnabled(false);
    loginButton_->setText(QStringLiteral("30 秒后重试"));
    lockTimer_->start();
}
} // namespace ncs::admin
