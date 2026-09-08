#pragma once
#include <QWidget>
class QLineEdit;
class QPushButton;
class QLabel;
class QTimer;
namespace ncs::admin
{
class LoginWidget final : public QWidget
{
    Q_OBJECT
  public:
    explicit LoginWidget(QWidget* parent = nullptr);
    void showError(const QString& message);
    void setBusy(bool busy);
    void clearPassword();
    void lockForThirtySeconds();
  signals:
    void loginRequested(const QString& username, const QString& password, const QString& deviceId);

  private:
    QLineEdit* usernameEdit_;
    QLineEdit* passwordEdit_;
    QPushButton* loginButton_;
    QLabel* errorLabel_;
    QTimer* lockTimer_;
    int lockRemaining_ = 0;
};
} // namespace ncs::admin
