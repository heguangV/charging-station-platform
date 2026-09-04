#pragma once

#include <QMainWindow>
#include <QList>

#include "admin_types.h"

class QListWidget;
class QStackedWidget;
class QTableWidget;
class QLabel;
class QLineEdit;
class QComboBox;

namespace ncs::admin
{

class AdminApiClient;
class LoginWidget;

class AdminMainWindow final : public QMainWindow
{
    Q_OBJECT

  public:
    explicit AdminMainWindow(QWidget* parent = nullptr);

  private slots:
    void handleLogin(const QString& username, const QString& password);
    void openWorkspace();
    void showApiError(const QString& message);
    void refreshStations();
    void refreshChargers();
    void refreshUsers();
    void addStation();
    void removeStation();
    void updateChargerStatus();
    void toggleUserStatus();

  private:
    void buildLoginPage();
    void buildWorkspace();
    QWidget* createDashboardPage();
    QWidget* createStationsPage();
    QWidget* createChargersPage();
    QWidget* createUsersPage();
    QWidget* createPredictionsPage();
    QWidget* createMetricCard(const QString& title, const QString& value, const QString& detail);
    void seedDemoData();
    void styleApplication();
    void setPage(int index);
    void fillStationTable();
    void fillChargerTable();
    void fillUserTable();

    LoginWidget* loginPage_;
    QStackedWidget* pages_;
    QStackedWidget* workspacePages_;
    QListWidget* navigation_;
    QTableWidget* stationTable_;
    QTableWidget* chargerTable_;
    QTableWidget* userTable_;
    QLineEdit* stationSearch_;
    QLineEdit* chargerSearch_;
    QLineEdit* userSearch_;
    QComboBox* chargerStatus_;
    QLabel* statusLabel_;
    AdminApiClient* apiClient_;
    QList<Station> stations_;
    QList<Charger> chargers_;
    QList<User> users_;
    QList<RevenuePoint> revenue_;
};

} // namespace ncs::admin
