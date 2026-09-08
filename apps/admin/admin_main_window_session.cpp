#include "admin_main_window.h"
#include "admin_main_window_utils.h"
#include "admin_page_status.h"
#include "login_widget.h"
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>
namespace ncs::admin
{
void AdminMainWindow::handleLogin(const QString& username, const QString& password,
                                  const QString& deviceId)
{
    loginPage_->setBusy(true);
    loginPage_->showError({});
    api_.postJson("admin/auth/login",
                  {{"username", username}, {"password", password}, {"deviceId", deviceId}},
                  [this](AdminReply reply)
                  {
                      loginPage_->setBusy(false);
                      if (!reply.ok())
                      {
                          loginPage_->showError(reply.message);
                          if (reply.httpStatus == 429)
                              loginPage_->lockForThirtySeconds();
                          return;
                      }
                      const auto data = reply.data.toObject();
                      const auto token = data.value("accessToken").toString();
                      const auto admin = data.value("admin").toObject();
                      if (token.isEmpty() || admin.value("username").toString().isEmpty())
                      {
                          loginPage_->showError(QStringLiteral("登录响应不完整，请重试"));
                          return;
                      }
                      api_.setAccessToken(token);
                      ++sessionGeneration_;
                      username_ = admin.value("username").toString();
                      passwordChangeRequired_ = admin.value("mustChangePassword").toBool();
                      loginPage_->clearPassword();
                      if (passwordChangeRequired_)
                          promptPasswordChange(true);
                      else
                          openWorkspace();
                  });
}
void AdminMainWindow::openWorkspace()
{
    loginPage_->setBusy(false);
    accountLabel_->setText(QStringLiteral("运营工作台  /  %1").arg(username_));
    pages_->setCurrentIndex(1);
    const QSignalBlocker blocker(navigation_);
    navigation_->setCurrentRow(0);
    workspacePages_->setCurrentIndex(0);
    notify({});
    connectionLabel_->setText(QStringLiteral("登录成功"));
    loadCatalog();
    refreshOverview();
}
void AdminMainWindow::returnToLogin(const QString& message)
{
    ++sessionGeneration_;
    api_.clearSession();
    reauthExpiresAt_ = 0;
    passwordChangeRequired_ = false;
    catalogLoading_ = false;
    catalog_.clear();
    username_.clear();
    for (int i = 0; i < 5; ++i)
    {
        beginPage(i);
        states_[i]->ready(QStringLiteral("等待登录"));
        pageNumbers_[i] = 1;
        updatePager(i, 0);
    }
    setBusy(false);
    pages_->setCurrentIndex(0);
    for (auto* dialog : findChildren<QDialog*>())
        dialog->reject();
    notify({});
    connectionLabel_->setText(QStringLiteral("未登录"));
    loginPage_->setBusy(false);
    loginPage_->clearPassword();
    loginPage_->showError(message);
}
void AdminMainWindow::logout()
{
    if (!api_.hasSession())
    {
        returnToLogin({});
        return;
    }
    loginPage_->setBusy(true);
    api_.postJson("admin/auth/logout", {},
                  [this](AdminReply reply)
                  {
                      returnToLogin(reply.ok()
                                        ? QStringLiteral("已退出登录")
                                        : QStringLiteral("已退出本机；暂时无法确认服务端退出状态"));
                  });
}
void AdminMainWindow::changePassword()
{
    if (api_.hasSession())
        promptPasswordChange(false);
}
void AdminMainWindow::promptPasswordChange(bool required)
{
    auto* dialog = new QDialog(this);
    dialog->setObjectName("changePasswordDialog");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(required ? QStringLiteral("首次登录，请修改密码")
                                    : QStringLiteral("修改登录密码"));
    dialog->setMinimumWidth(420);
    dialog->setModal(true);
    auto* form = new QFormLayout(dialog);
    form->setContentsMargins(24, 24, 24, 24);
    form->setSpacing(14);
    auto* hint = new QLabel(required ? QStringLiteral("完成密码修改后即可进入运营中心")
                                     : QStringLiteral("修改后保留本机登录，其他终端需重新登录"));
    hint->setWordWrap(true);
    form->addRow(hint);
    auto* current = new QLineEdit;
    current->setObjectName("currentPassword");
    auto* password = new QLineEdit;
    password->setObjectName("newPassword");
    auto* confirm = new QLineEdit;
    confirm->setObjectName("confirmPassword");
    for (auto* field : {current, password, confirm})
    {
        field->setEchoMode(QLineEdit::Password);
        field->setMaxLength(128);
    }
    form->addRow(QStringLiteral("当前密码"), current);
    form->addRow(QStringLiteral("新密码"), password);
    form->addRow(QStringLiteral("再次输入"), confirm);
    auto* error = new QLabel;
    error->setObjectName("passwordError");
    error->setTextFormat(Qt::PlainText);
    error->setWordWrap(true);
    error->setStyleSheet("color:#B42318;");
    form->addRow(error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存密码"));
    buttons->button(QDialogButtonBox::Cancel)
        ->setText(required ? QStringLiteral("退出登录") : QStringLiteral("取消"));
    form->addRow(buttons);
    QPointer<QDialog> guard(dialog);
    connect(
        buttons, &QDialogButtonBox::accepted, this,
        [this, guard, current, password, confirm, error, buttons, required]
        {
            if (current->text().isEmpty() || password->text().size() < 10 ||
                password->text() != confirm->text())
            {
                error->setText(QStringLiteral("请填写当前密码；新密码至少 10 位，且两次输入一致"));
                return;
            }
            buttons->setEnabled(false);
            error->setText(QStringLiteral("正在保存…"));
            api_.putJson("admin/me/password",
                         {{"currentPassword", current->text()}, {"newPassword", password->text()}},
                         [this, guard, error, buttons, required](AdminReply reply)
                         {
                             if (!guard)
                                 return;
                             buttons->setEnabled(true);
                             if (!reply.ok())
                             {
                                 error->setText(reply.message);
                                 return;
                             }
                             passwordChangeRequired_ = false;
                             reauthExpiresAt_ = 0;
                             guard->accept();
                             if (required)
                                 openWorkspace();
                             else
                                 notify(QStringLiteral("密码已更新，其他终端需要重新登录"));
                         });
        });
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(dialog, &QDialog::rejected, this,
            [this, required]
            {
                if (required && api_.hasSession())
                    logout();
            });
    dialog->open();
}
void AdminMainWindow::reauthenticate(std::function<void()> done)
{
    if (reauthExpiresAt_ > QDateTime::currentSecsSinceEpoch() + 5)
    {
        done();
        return;
    }
    bool ok = false;
    QString password = QInputDialog::getText(this, QStringLiteral("管理员身份验证"),
                                             QStringLiteral("请输入当前登录密码以继续维护操作"),
                                             QLineEdit::Password, {}, &ok);
    if (!ok)
        return;
    if (password.isEmpty())
    {
        notify(QStringLiteral("请输入密码后重试"), true);
        return;
    }
    setBusy(true);
    api_.postJson("admin/auth/reauth", {{"password", password}},
                  [this, done = std::move(done)](AdminReply reply)
                  {
                      setBusy(false);
                      if (!reply.ok())
                      {
                          notify(reply.message, true);
                          return;
                      }
                      reauthExpiresAt_ = reply.data.toObject().value("reauthExpiresAt").toInteger();
                      if (reauthExpiresAt_ <= QDateTime::currentSecsSinceEpoch())
                      {
                          notify(QStringLiteral("身份验证结果已失效，请重试"), true);
                          return;
                      }
                      done();
                  });
    password.fill(QChar(0));
    password.clear();
}
} // namespace ncs::admin
