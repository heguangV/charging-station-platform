#pragma once

#include <QString>
#include <QVector>

namespace ncs::user
{

struct StationSummary
{
    int id = 0;
    QString name;
    QString address;
    int priceCentPerKwh = 0;
    int idleCount = 0;
    int totalCount = 0;
    QString distance;
};

struct ChargerSummary
{
    QString code;
    QString type;
    int powerKw = 0;
    QString status;
    int totalCount = 0;
};

struct ChargeProgress
{
    int durationSeconds = 0;
    int energyMwh = 0;
    int amountCent = 0;
    int powerKw = 0;
    int soc = 0;
};

struct OrderSummary
{
    QString orderNo;
    QString stationName;
    QString chargerCode;
    QString startTime;
    QString endTime;
    int energyMwh = 0;
    int amountCent = 0;
    int durationSeconds = 0;
    QString status;
};

struct NavigationRoute
{
    QString stationName;
    QString destinationAddress;
    QString distance;
    QString mode;
    QString url;
};

class UserClientService
{
  public:
    virtual ~UserClientService() = default;
    virtual bool login(const QString& phone, const QString& code, QString* userMessage) = 0;
    virtual QString developmentCode() const = 0;
    virtual QString nickname() const = 0;
    virtual QString phoneMasked() const = 0;
    virtual QString avatarPath() const = 0;
    virtual int balanceCent() const = 0;
    virtual bool updateNickname(const QString& nickname, QString* userMessage) = 0;
    virtual bool updateAvatar(const QString& filePath, QString* userMessage) = 0;
    virtual bool logout(QString* userMessage) = 0;
    virtual QVector<OrderSummary> orders() const = 0;
    virtual NavigationRoute route(int stationId, const QString& mode) const = 0;
    virtual QVector<StationSummary> stations() const = 0;
    virtual QVector<ChargerSummary> chargers(int stationId) const = 0;
    virtual bool reserve(int stationId, const QString& chargerCode, QString* userMessage) = 0;
    virtual bool cancelReservation(QString* userMessage) = 0;
    virtual int reservationRemainingSeconds() const = 0;
    virtual bool hasUnfinishedOrder() const = 0;
    virtual bool start(QString* userMessage) = 0;
    virtual void tick() = 0;
    virtual ChargeProgress progress() const = 0;
    virtual bool settle(QString* userMessage) = 0;
    virtual bool recharge(int amountCent, QString* userMessage) = 0;
};

class MockUserClientService final : public UserClientService
{
  public:
    bool login(const QString& phone, const QString& code, QString* userMessage) override;
    QString developmentCode() const override;
    QString nickname() const override;
    QString phoneMasked() const override;
    QString avatarPath() const override;
    int balanceCent() const override;
    bool updateNickname(const QString& nickname, QString* userMessage) override;
    bool updateAvatar(const QString& filePath, QString* userMessage) override;
    bool logout(QString* userMessage) override;
    QVector<OrderSummary> orders() const override;
    NavigationRoute route(int stationId, const QString& mode) const override;
    QVector<StationSummary> stations() const override;
    QVector<ChargerSummary> chargers(int stationId) const override;
    bool reserve(int stationId, const QString& chargerCode, QString* userMessage) override;
    bool cancelReservation(QString* userMessage) override;
    int reservationRemainingSeconds() const override;
    bool hasUnfinishedOrder() const override;
    bool start(QString* userMessage) override;
    void tick() override;
    ChargeProgress progress() const override;
    bool settle(QString* userMessage) override;
    bool recharge(int amountCent, QString* userMessage) override;

  private:
    QString phone_;
    QString nickname_;
    QString avatarPath_;
    int balanceCent_ = 12800;
    int selectedStationId_ = 0;
    QString selectedChargerCode_;
    bool reserved_ = false;
    bool charging_ = false;
    int reservationRemainingSeconds_ = 0;
    int elapsedSeconds_ = 0;
    QString activeOrderNo_;
    QVector<OrderSummary> orders_;
};

} // namespace ncs::user
