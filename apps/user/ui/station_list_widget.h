#pragma once

#include "../user_demo_service.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

namespace ncs::user
{

class StationListWidget final : public QWidget
{
    Q_OBJECT

  public:
    explicit StationListWidget(const QVector<StationSummary>& stations, QWidget* parent = nullptr);
    void setStations(QVector<StationSummary> stations);
    void setRemoteSource(bool enabled);
    void setLoading(bool loading);
    void showError(const QString& userMessage);

  signals:
    void stationSelected(const StationSummary& station);
    void loadRequested(qint64 latitudeE6, qint64 longitudeE6, const QString& locationKeyword);

  private:
    void clearCards();
    void refresh();
    void requestRefresh();

    QVector<StationSummary> stations_;
    QComboBox* locationBox_ = nullptr;
    QLineEdit* searchEdit_ = nullptr;
    QLabel* summary_ = nullptr;
    QVBoxLayout* cards_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    bool remoteSource_ = false;
};

} // namespace ncs::user
