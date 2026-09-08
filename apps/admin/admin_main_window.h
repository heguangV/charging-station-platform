#pragma once
#include "admin_api_client.h"
#include "admin_types.h"
#include <QHash>
#include <QJsonArray>
#include <QMainWindow>
#include <array>
#include <functional>
class QTableWidget;
class QStackedWidget;
class QListWidget;
class QLabel;
class QLineEdit;
class QComboBox;
class QPushButton;
class QVBoxLayout;
class QGridLayout;
namespace ncs::admin
{
class LoginWidget;
class AdminPageStatus;
class AdminTrendWidget;
class AdminStatusChart;
class AdminMainWindow final : public QMainWindow
{
    Q_OBJECT
  public:
    explicit AdminMainWindow(AdminApiClient& api, const QString& environment,
                             QWidget* parent = nullptr);
  private slots:
    void handleLogin(const QString& username, const QString& password, const QString& deviceId);
    void refreshCurrentPage();
    void refreshOverview();
    void refreshStations();
    void refreshChargers();
    void refreshUsers();
    void refreshPredictions();
    void addStation();
    void removeStation();
    void updateChargerStatus();
    void toggleUserStatus();
    void restartCharger();
    void logout();
    void changePassword();
    void runPrediction();

  private:
    void buildWorkspace(const QString& environment);
    void styleApplication();
    void openWorkspace();
    void returnToLogin(const QString& message);
    void promptPasswordChange(bool required);
    void reauthenticate(std::function<void()> done);
    void pollCommand(const QString& commandNo, int attempts);
    void pollPrediction(const QString& taskNo, int attempts);
    QWidget* newPage(int index, const QString& title, const QString& subtitle,
                     QVBoxLayout** layout);
    QWidget* createDashboardPage();
    QWidget* createStationsPage();
    QWidget* createChargersPage();
    QWidget* createUsersPage();
    QWidget* createPredictionsPage();
    QWidget* metric(const QString& title, const QString& detail, const QString& name, QLabel** out);
    void addPager(QVBoxLayout* layout, int page);
    quint64 beginPage(int page);
    void failPage(int page, const QString& message);
    void loadList(int page, const QString& path, const QUrlQuery& query,
                  std::function<void(const QJsonObject&)> done);
    void loadCatalog(int page = 1, QList<Station> accumulated = {});
    void updateStationChoices();
    QString stationName(qint64 id) const;
    void fillStationTable();
    void fillChargerTable();
    void fillUserTable();
    void setBusy(bool busy);
    void notify(const QString& text, bool error = false);
    void mutate(const QString& method, const QString& path, const QJsonObject& body, int page);
    qint64 selectedId(QTableWidget* table) const;
    bool confirmReason(const QString& title, const QString& description, QString* reason);
    void updatePager(int page, int total);
    AdminApiClient& api_;
    LoginWidget* loginPage_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QStackedWidget* workspacePages_ = nullptr;
    QListWidget* navigation_ = nullptr;
    QLabel* accountLabel_ = nullptr;
    QLabel* connectionLabel_ = nullptr;
    QLabel* operationMessage_ = nullptr;
    QString username_;
    quint64 sessionGeneration_ = 0;
    qint64 reauthExpiresAt_ = 0;
    bool operationBusy_ = false;
    bool passwordChangeRequired_ = false;
    std::array<AdminPageStatus*, 5> states_{};
    std::array<quint64, 5> generations_{};
    std::array<int, 5> pageNumbers_{{1, 1, 1, 1, 1}};
    std::array<QPushButton*, 5> previous_{};
    std::array<QPushButton*, 5> next_{};
    std::array<QLabel*, 5> pageLabels_{};
    QList<QPushButton*> mutationButtons_;
    QTableWidget* stationTable_ = nullptr;
    QTableWidget* chargerTable_ = nullptr;
    QTableWidget* userTable_ = nullptr;
    QTableWidget* predictionTable_ = nullptr;
    QTableWidget* revenueTable_ = nullptr;
    QLineEdit* stationSearch_ = nullptr;
    QLineEdit* chargerSearch_ = nullptr;
    QLineEdit* userSearch_ = nullptr;
    QComboBox* chargerStatus_ = nullptr;
    QComboBox* chargerStation_ = nullptr;
    QComboBox* predictionStation_ = nullptr;
    QComboBox* predictionHorizon_ = nullptr;
    QComboBox* revenueRange_ = nullptr;
    QLabel* todayRevenue_ = nullptr;
    QLabel* monthRevenue_ = nullptr;
    QLabel* operationalChargers_ = nullptr;
    QLabel* registeredUsers_ = nullptr;
    QLabel* health_ = nullptr;
    AdminTrendWidget* trend_ = nullptr;
    AdminStatusChart* statusChart_ = nullptr;
    QList<Station> stations_, catalog_;
    QList<Charger> chargers_;
    QList<User> users_;
    QList<PredictionPoint> predictions_;
    bool catalogLoading_ = false;
};
} // namespace ncs::admin
