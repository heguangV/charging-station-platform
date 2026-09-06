#pragma once

#include "user_demo_service.h"

#include <QHash>
#include <QMainWindow>
#include <QVector>

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QTimer;
class QComboBox;
class QScrollArea;
class QVBoxLayout;
class QPauseAnimation;
class QSequentialAnimationGroup;
class QGraphicsOpacityEffect;
class QJsonObject;
namespace ncs::user
{
class ChargerTable;
}
namespace ncs::user
{
class ChargeSocGauge;
}
namespace ncs::user
{
class StationListWidget;
}
namespace ncs::user
{
class BottomNavigation;
}
namespace ncs::user
{
class UserApi;
}
namespace ncs::user
{

class UserMainWindow final : public QMainWindow
{
  public:
    explicit UserMainWindow(UserClientService& service, UserApi* userApi, QString tencentMapJsKey,
                            QString tencentMapJsOrigin, QWidget* parent = nullptr);

  private:
    QWidget* createLoginPage();
    QWidget* createHomePage();
    QWidget* createDetailPage();
    QWidget* createChargePage();
    QWidget* createReceiptPage();
    QWidget* createProfilePage();
    QWidget* createOrdersPage();
    QWidget* createNavigationPage();
    void showLogin();
    void showOrders();
    void showProfile();
    void showNavigation();
    void showNavigationFallback(const QString& reason = {});
    void applyNavigationRoute(const QJsonObject& routeData);
    void renderNavigationMap(const QJsonObject& routeData);
    void showHome();
    void showDetail(int stationId);
    void showCharge();
    void refreshCharge();
    void restoreActiveFlow();
    void restoreFlow(const QJsonObject& flow);
    void beginFlowRequest();
    void refreshProfile();
    void refreshOrders();
    void renderOrders(const QVector<OrderSummary>& records);
    void notify(const QString& message, bool error = false);
    static QString money(int cent);
    static QPushButton* button(const QString& text, const QString& style = {});

    UserClientService& service_;
    UserApi* userApi_ = nullptr;
    QString tencentMapJsKey_;
    QString tencentMapJsOrigin_;
    bool onlineSession_ = false;
    int navigationRequestId_ = 0;
    QStackedWidget* pages_ = nullptr;
    QLabel* notice_ = nullptr;
    QGraphicsOpacityEffect* noticeOpacity_ = nullptr;
    QSequentialAnimationGroup* noticeAnimation_ = nullptr;
    QPauseAnimation* noticePause_ = nullptr;
    QLineEdit* phoneEdit_ = nullptr;
    QLineEdit* codeEdit_ = nullptr;
    QPushButton* codeButton_ = nullptr;
    QLabel* detailTitle_ = nullptr;
    QLabel* detailMeta_ = nullptr;
    ChargerTable* chargerTable_ = nullptr;
    StationListWidget* stationList_ = nullptr;
    BottomNavigation* bottomNavigation_ = nullptr;
    QLabel* chargeState_ = nullptr;
    QLabel* reservationCountdown_ = nullptr;
    QLabel* chargeDuration_ = nullptr;
    QLabel* chargeEnergy_ = nullptr;
    QLabel* chargeAmount_ = nullptr;
    QLabel* chargePower_ = nullptr;
    ChargeSocGauge* chargeSoc_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    QPushButton* settleButton_ = nullptr;
    QLabel* receiptText_ = nullptr;
    QLabel* profileName_ = nullptr;
    QLabel* profileAvatar_ = nullptr;
    QLabel* profileBalance_ = nullptr;
    QLineEdit* nicknameEdit_ = nullptr;
    QScrollArea* ordersScroll_ = nullptr;
    QVBoxLayout* ordersCards_ = nullptr;
    QLabel* ordersEmpty_ = nullptr;
    QComboBox* navigationMode_ = nullptr;
    QLabel* navigationSummary_ = nullptr;
    QWidget* navigationMap_ = nullptr;
    QPushButton* navigationBrowserButton_ = nullptr;
    NavigationRoute navigationRoute_;
    QTimer* timer_ = nullptr;
    int selectedStationId_ = 1;
    QString selectedChargerCode_;
    QString selectedStationDistance_;
    qint64 selectedChargerId_ = 0;
    int selectedChargerType_ = 0;
    QString activeFlowNo_;
    qint64 activeFlowVersion_ = 0;
    qint64 reservationUntil_ = 0;
    int activeFlowStatus_ = 0;
    int flowPollTicks_ = 0;
    bool progressRequestInFlight_ = false;
    QHash<int, StationSummary> stationsById_;
    QVector<OrderSummary> orderRecords_;
    qint64 profileVersion_ = 0;
    bool chargingStarted_ = false;
    int codeCountdown_ = 0;

  protected:
    void keyPressEvent(QKeyEvent* event) override;
};

} // namespace ncs::user
