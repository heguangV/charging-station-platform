#pragma once

#include <QWidget>

class QLineEdit;
class QPushButton;
class QLabel;

namespace ncs::admin
{

class LoginWidget final : public QWidget
{
    Q_OBJECT

  public:
    explicit LoginWidget(QWidget* parent = nullptr);

  signals:
    void loginRequested(const QString& username, const QString& password, const QString& deviceId);

  public slots:
    void showError(const QString& message);
    void setBusy(bool busy);

  private:
    QLineEdit* usernameEdit_;
    QLineEdit* passwordEdit_;
    QPushButton* loginButton_;
    QLabel* errorLabel_;
};

} // namespace ncs::admin
