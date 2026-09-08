#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <functional>

namespace ncs::mobile
{
class MobileApi final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool locating READ locating NOTIFY locationChanged)
    Q_PROPERTY(QString serverUrl READ serverUrl NOTIFY serverChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool avatarUploading READ avatarUploading NOTIFY busyChanged)
    Q_PROPERTY(QString locationLabel READ locationLabel NOTIFY locationChanged)
    Q_PROPERTY(QString createdAt READ createdAt NOTIFY profileChanged)
    Q_PROPERTY(bool loggedIn READ loggedIn NOTIFY sessionChanged)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    Q_PROPERTY(bool messageError READ messageError NOTIFY messageChanged)
    Q_PROPERTY(QVariantList stations READ stations NOTIFY stationsChanged)
    Q_PROPERTY(QVariantList orders READ orders NOTIFY ordersChanged)
    Q_PROPERTY(QString nickname READ nickname NOTIFY profileChanged)
    Q_PROPERTY(QString phone READ phone NOTIFY profileChanged)
    Q_PROPERTY(qint64 balanceCent READ balanceCent NOTIFY profileChanged)
    Q_PROPERTY(QString avatarData READ avatarData NOTIFY avatarChanged)
    Q_PROPERTY(QVariantMap flow READ flow NOTIFY flowChanged)
    Q_PROPERTY(QVariantList chargers READ chargers NOTIFY chargersChanged)
    Q_PROPERTY(QVariantMap receipt READ receipt NOTIFY receiptChanged)
    Q_PROPERTY(QVariantMap route READ route NOTIFY routeChanged)
    Q_PROPERTY(QVariantMap reviewInfo READ reviewInfo NOTIFY reviewChanged)
    Q_PROPERTY(QVariantList stationReviews READ stationReviews NOTIFY stationReviewsChanged)
    Q_PROPERTY(bool stationReviewsBusy READ stationReviewsBusy NOTIFY stationReviewsChanged)
    Q_PROPERTY(QString stationReviewsError READ stationReviewsError NOTIFY stationReviewsChanged)
  public:
    explicit MobileApi(QObject* parent = nullptr);
    bool locating() const
    {
        return locating_;
    }
    Q_INVOKABLE void locateDevice();
    QString serverUrl() const
    {
        return baseUrl_;
    }
    bool busy() const
    {
        return pending_ > 0;
    }
    bool avatarUploading() const
    {
        return avatarUploading_;
    }
    QString locationLabel() const
    {
        return locationLabel_;
    }
    QString createdAt() const
    {
        return createdAt_;
    }
    Q_INVOKABLE bool configureServer(const QString& url);
    Q_INVOKABLE void updateNickname(const QString& nickname);
    Q_INVOKABLE void recharge(const QString& amount);
    Q_INVOKABLE void requestDeletionCode();
    Q_INVOKABLE void deleteAccount(const QString& code);
    Q_INVOKABLE void loadOrder(const QString& orderNo);
    Q_INVOKABLE void setLocation(int region, const QString& address);
    Q_INVOKABLE void discardCapture();
    bool loggedIn() const
    {
        return !token_.isEmpty();
    }
    QString message() const
    {
        return message_;
    }
    bool messageError() const
    {
        return messageError_;
    }
    QVariantList stations() const
    {
        return stations_;
    }
    QVariantList orders() const
    {
        return orders_;
    }
    QString nickname() const
    {
        return nickname_;
    }
    QString phone() const
    {
        return phone_;
    }
    qint64 balanceCent() const
    {
        return balanceCent_;
    }
    QString avatarData() const
    {
        return avatarData_;
    }
    QVariantMap flow() const
    {
        return flow_;
    }
    QVariantList chargers() const
    {
        return chargers_;
    }
    QVariantMap receipt() const
    {
        return receipt_;
    }
    QVariantMap route() const
    {
        return route_;
    }
    // UC-U-12 订单评价状态：orderNo/existing/submitted/busy/rating/content/error。
    QVariantMap reviewInfo() const
    {
        return reviewInfo_;
    }
    QVariantList stationReviews() const
    {
        return stationReviews_;
    }
    bool stationReviewsBusy() const
    {
        return stationReviewsBusy_;
    }
    QString stationReviewsError() const
    {
        return stationReviewsError_;
    }
    Q_INVOKABLE void requestCode(const QString& phone);
    Q_INVOKABLE void login(const QString& phone, const QString& code);
    Q_INVOKABLE void loadStations(const QString& keyword = {});
    Q_INVOKABLE void loadProfile();
    Q_INVOKABLE void loadOrders();
    Q_INVOKABLE void loadStationChargers(qint64 stationId);
    Q_INVOKABLE void uploadAvatar(const QString& path);
    Q_INVOKABLE void requestCharge(qint64 stationId, int chargerType = 1,
                                   qint64 preferredChargerId = 0);
    Q_INVOKABLE void confirmCharge();
    Q_INVOKABLE void startCharge();
    Q_INVOKABLE void settleCharge();
    Q_INVOKABLE void cancelCharge();
    Q_INVOKABLE void loadActiveFlow();
    Q_INVOKABLE void loadFlowProgress();
    Q_INVOKABLE void loadRoute(qint64 stationId, const QString& mode);
    Q_INVOKABLE void logout();
    Q_INVOKABLE void beginReview(const QString& orderNo);
    Q_INVOKABLE void submitReview(int rating, const QString& content);
    // UC-U-12 场站评论墙：只读，作者已在服务端脱敏。
    Q_INVOKABLE void loadStationReviews(qint64 stationId);
    // Android 6.0+ CAMERA 属运行时危险权限，拍摄前须查询/请求。
    Q_INVOKABLE bool cameraPermissionGranted() const;
    Q_INVOKABLE void requestCameraPermission();
    Q_INVOKABLE QString avatarCapturePath() const;
  signals:
    void serverChanged();
    void busyChanged();
    void locationChanged();
    void codeSent(int seconds);
    void sessionChanged();
    void messageChanged();
    void stationsChanged();
    void ordersChanged();
    void profileChanged();
    void avatarChanged();
    void flowChanged();
    void chargersChanged();
    void receiptChanged();
    void routeChanged();
    void reviewChanged();
    void stationReviewsChanged();
    void cameraPermissionResult(bool granted);

  private:
    QNetworkRequest
    request(const QString& path,
            const QByteArray& contentType = QByteArrayLiteral("application/json")) const;
    void get(const QString& path, std::function<void(const QJsonObject&)> done,
             std::function<void(const QString&)> fail = {});
    void setMessage(const QString& value, bool error = false);
    void parseReply(QNetworkReply* reply, std::function<void(const QJsonObject&)> done,
                    const std::function<void(const QString&)>& fail = {});
    void postJson(const QString& path, const QJsonObject& body,
                  std::function<void(const QJsonObject&)> done);
    void handleReviewReply(QNetworkReply* reply, bool submit, int rating, const QString& content);
    void setReviewError(const QString& message);
    void clearSession();
    void watch(QNetworkReply* reply);
    bool locating_ = false;
    int locationRequest_ = 0;
    int stationRequest_ = 0, chargerRequest_ = 0, routeRequest_ = 0, reviewRequest_ = 0,
        wallRequest_ = 0;
    int pending_ = 0;
    int generation_ = 0;
    bool avatarUploading_ = false;
    qint64 profileVersion_ = 0;
    qint64 latitudeE6_ = 39904200, longitudeE6_ = 116407400;
    QString locationLabel_ = QStringLiteral("北京中心（模拟位置）");
    QString locationAddress_, loginPhone_, createdAt_;
    QHash<QString, QByteArray> mutationKeys_;
    QNetworkAccessManager manager_;
    QString baseUrl_;
    QString token_;
    QString message_;
    bool messageError_ = false;
    QVariantList stations_;
    QVariantList orders_;
    QVariantList chargers_;
    QVariantMap receipt_;
    QVariantMap route_;
    QString nickname_;
    QString phone_;
    QString avatarData_;
    qint64 balanceCent_ = 0;
    QVariantMap flow_;
    QString reviewOrderNo_;
    QString reviewKey_;
    QVariantMap reviewInfo_;
    QVariantList stationReviews_;
    QString stationReviewsError_;
    bool stationReviewsBusy_ = false;
};
} // namespace ncs::mobile
