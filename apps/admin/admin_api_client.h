#pragma once

#include <QObject>
#include <QUrl>

class QNetworkAccessManager;

namespace ncs::admin
{

class AdminApiClient final : public QObject
{
    Q_OBJECT

  public:
    explicit AdminApiClient(QObject* parent = nullptr);

    void setBaseUrl(const QUrl& baseUrl);
    void login(const QString& username, const QString& password);

  signals:
    void loginSucceeded(const QString& token);
    void requestFailed(const QString& message);

  private:
    QNetworkAccessManager* manager_;
    QUrl baseUrl_;
};

} // namespace ncs::admin
