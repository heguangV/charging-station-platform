#include "login_widget.h"

#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace ncs::admin
{

LoginWidget::LoginWidget(QWidget* parent)
    : QWidget(parent), usernameEdit_(new QLineEdit(this)), passwordEdit_(new QLineEdit(this)),
      loginButton_(new QPushButton(QStringLiteral("登录管理端"), this)), errorLabel_(new QLabel(this))
{
    setObjectName(QStringLiteral("loginPage"));
    setMinimumSize(460, 360);

    auto* title = new QLabel(QStringLiteral("NCS 运营管理端"), this);
    title->setObjectName(QStringLiteral("loginTitle"));
    auto* subtitle = new QLabel(QStringLiteral("充电站平台运营控制台"), this);
    subtitle->setObjectName(QStringLiteral("loginSubtitle"));

    usernameEdit_->setPlaceholderText(QStringLiteral("请输入管理员账号"));
    passwordEdit_->setPlaceholderText(QStringLiteral("请输入密码"));
    passwordEdit_->setEchoMode(QLineEdit::Password);
    errorLabel_->setObjectName(QStringLiteral("loginError"));
    errorLabel_->setWordWrap(true);

    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignLeft);
    form->setFormAlignment(Qt::AlignHCenter);
    form->setVerticalSpacing(14);
    form->addRow(QStringLiteral("账号"), usernameEdit_);
    form->addRow(QStringLiteral("密码"), passwordEdit_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(54, 48, 54, 48);
    layout->setSpacing(16);
    layout->addWidget(title);
    layout->addWidget(subtitle);
    layout->addSpacing(16);
    layout->addLayout(form);
    layout->addWidget(errorLabel_);
    layout->addWidget(loginButton_);
    layout->addStretch();

    connect(loginButton_, &QPushButton::clicked, this, [this] {
        if (usernameEdit_->text().trimmed().isEmpty() || passwordEdit_->text().isEmpty()) {
            showError(QStringLiteral("请输入账号和密码"));
            return;
        }
        emit loginRequested(usernameEdit_->text().trimmed(), passwordEdit_->text(),
                            QStringLiteral("admin-desktop-a1"));
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
    loginButton_->setEnabled(!busy);
    loginButton_->setText(busy ? QStringLiteral("正在登录...") : QStringLiteral("登录管理端"));
}

} // namespace ncs::admin
