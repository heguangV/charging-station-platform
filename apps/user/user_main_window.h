#pragma once

#include "user_demo_service.h"

#include <QMainWindow>

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QTimer;
class QTableWidget;
class QComboBox;
class QPauseAnimation;
class QSequentialAnimationGroup;
class QGraphicsOpacityEffect;
namespace ncs::user { class ChargerTable; }
namespace ncs::user { class ChargeSocGauge; }
namespace ncs::user { class StationListWidget; }
namespace ncs::user { class BottomNavigation; }
namespace ncs::user
{

class UserMainWindow final : public QMainWindow
{
  public:
    explicit UserMainWindow(UserClientService& service, QWidget* parent = nullptr);

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
    void showHome();
    void showDetail(int stationId);
    void showCharge();
    void refreshCharge();
    void refreshProfile();
    void refreshOrders();
    void notify(const QString& message, bool error = false);
    static QString money(int cent);
    static QPushButton* button(const QString& text, const QString& style = {});

    UserClientService& service_;
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
    QTableWidget* ordersTable_ = nullptr;
    QLabel* ordersEmpty_ = nullptr;
    QComboBox* navigationMode_ = nullptr;
    QLabel* navigationSummary_ = nullptr;
    NavigationRoute navigationRoute_;
    QTimer* timer_ = nullptr;
    int selectedStationId_ = 1;
    QString selectedChargerCode_;
    bool chargingStarted_ = false;
    int codeCountdown_ = 0;

  protected:
    void keyPressEvent(QKeyEvent* event) override;
};

} // namespace ncs::user
