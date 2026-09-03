#include "user_demo_service.h"

#include <algorithm>
#include <QFileInfo>
#include <QImageReader>
#include <QDateTime>
#include <QUrl>
#include <QUrlQuery>

namespace ncs::user
{

bool MockUserClientService::configureScenario(const QString& name, QString* userMessage)
{
    resetScenario();
    const QString scenario = name.trimmed().toLower();
    if (scenario.isEmpty() || scenario == QStringLiteral("happy-path") ||
        scenario == QStringLiteral("empty-orders"))
    {
        *userMessage = QStringLiteral("Mock 场景：正常充电流程");
        return true;
    }
    if (scenario == QStringLiteral("low-balance"))
    {
        balanceCent_ = 100;
        *userMessage = QStringLiteral("Mock 场景：余额不足");
        return true;
    }
    if (scenario == QStringLiteral("no-available-charger"))
    {
        noAvailableChargers_ = true;
        *userMessage = QStringLiteral("Mock 场景：当前站点无空闲电桩");
        return true;
    }
    if (scenario == QStringLiteral("active-charging"))
    {
        selectedStationId_ = 1;
        selectedChargerCode_ = QStringLiteral("ZGC-DC-01");
        activeOrderNo_ = QStringLiteral("OR-DEMO-ACTIVE");
        charging_ = true;
        reserved_ = true;
        elapsedSeconds_ = 42;
        orders_.append({activeOrderNo_, QStringLiteral("NCS 中关村充电站"), selectedChargerCode_,
                        QStringLiteral("2026-09-03 14:00:00"), {}, 0, 0, 0,
                        QStringLiteral("充电中")});
        *userMessage = QStringLiteral("Mock 场景：存在充电中订单");
        return true;
    }
    *userMessage = QStringLiteral("未知 Mock 场景：%1（可选 happy-path、low-balance、no-available-charger、active-charging）")
                       .arg(name);
    return false;
}

void MockUserClientService::resetScenario()
{
    balanceCent_ = 12800;
    selectedStationId_ = 0;
    selectedChargerCode_.clear();
    reserved_ = false;
    charging_ = false;
    reservationRemainingSeconds_ = 0;
    elapsedSeconds_ = 0;
    activeOrderNo_.clear();
    orders_.clear();
    noAvailableChargers_ = false;
}

bool MockUserClientService::login(const QString& phone, const QString& code, QString* userMessage)
{
    if (code != developmentCode())
    {
        *userMessage = QStringLiteral("验证码错误，请输入演示验证码 123456");
        return false;
    }
    phone_ = phone;
    if (nickname_.isEmpty())
    {
        nickname_ = QStringLiteral("用户%1").arg(phone.right(4));
    }
    *userMessage = QStringLiteral("登录成功，已进入演示数据模式");
    return true;
}

QString MockUserClientService::developmentCode() const
{
    return QStringLiteral("123456");
}

QString MockUserClientService::nickname() const
{
    return nickname_.isEmpty() ? QStringLiteral("用户8000") : nickname_;
}

QString MockUserClientService::phoneMasked() const
{
    return phone_.isEmpty() ? QStringLiteral("138****8000")
                            : phone_.left(3) + QStringLiteral("****") + phone_.right(4);
}

int MockUserClientService::balanceCent() const
{
    return balanceCent_;
}

QString MockUserClientService::avatarPath() const { return avatarPath_; }

bool MockUserClientService::updateNickname(const QString& nickname, QString* userMessage)
{
    const QString normalized = nickname.trimmed();
    if (normalized.isEmpty() || normalized.size() > 20)
    {
        *userMessage = QStringLiteral("昵称应为 1 至 20 个非空白字符");
        return false;
    }
    nickname_ = normalized;
    *userMessage = QStringLiteral("昵称已保存");
    return true;
}

bool MockUserClientService::updateAvatar(const QString& filePath, QString* userMessage)
{
    const QFileInfo file(filePath);
    QImageReader reader(filePath);
    if (!file.exists() || file.size() > 5 * 1024 * 1024 || !reader.canRead())
    {
        *userMessage = QStringLiteral("请选择 5MB 以内的 PNG、JPG、JPEG 或 BMP 图片");
        return false;
    }
    avatarPath_ = filePath;
    *userMessage = QStringLiteral("头像已更新");
    return true;
}

bool MockUserClientService::logout(QString* userMessage)
{
    phone_.clear();
    reserved_ = false;
    charging_ = false;
    reservationRemainingSeconds_ = 0;
    *userMessage = QStringLiteral("已退出登录");
    return true;
}

QVector<OrderSummary> MockUserClientService::orders() const { return orders_; }

NavigationRoute MockUserClientService::route(int stationId, const QString& mode) const
{
    const StationSummary station = stations().at(qBound(0, stationId - 1, stations().size() - 1));
    const QString routeType = mode == QStringLiteral("walking") ? QStringLiteral("walk")
                                                                 : QStringLiteral("drive");
    QUrl url(QStringLiteral("https://apis.map.qq.com/uri/v1/routeplan"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("type"), routeType);
    query.addQueryItem(QStringLiteral("from"), QStringLiteral("当前位置"));
    query.addQueryItem(QStringLiteral("to"), station.name);
    url.setQuery(query);
    return {station.name, station.address, station.distance, mode, url.toString()};
}

QVector<StationSummary> MockUserClientService::stations() const
{
    return {{1, QStringLiteral("NCS 中关村充电站"), QStringLiteral("海淀区中关村大街 27 号"),
             140, 5, 12, QStringLiteral("1.8 km")},
            {2, QStringLiteral("望京智充站"), QStringLiteral("朝阳区阜通东大街 6 号"), 135, 3, 10,
             QStringLiteral("4.6 km")},
            {3, QStringLiteral("国贸商务区站"), QStringLiteral("朝阳区建国门外大街 1 号"), 150, 2, 8,
             QStringLiteral("7.2 km")}};
}

QVector<ChargerSummary> MockUserClientService::chargers(int stationId) const
{
    Q_UNUSED(stationId)
    if (noAvailableChargers_)
    {
        return {{QStringLiteral("ZGC-DC-01"), QStringLiteral("直流快充"), 120, QStringLiteral("充电中"), 248},
                {QStringLiteral("ZGC-DC-02"), QStringLiteral("直流快充"), 60, QStringLiteral("充电中"), 176},
                {QStringLiteral("ZGC-AC-03"), QStringLiteral("交流慢充"), 7, QStringLiteral("故障"), 93}};
    }
    return {{QStringLiteral("ZGC-DC-01"), QStringLiteral("直流快充"), 120, QStringLiteral("空闲"), 248},
            {QStringLiteral("ZGC-DC-02"), QStringLiteral("直流快充"), 60, QStringLiteral("充电中"), 176},
            {QStringLiteral("ZGC-AC-03"), QStringLiteral("交流慢充"), 7, QStringLiteral("空闲"), 93},
            {QStringLiteral("ZGC-DC-04"), QStringLiteral("直流快充"), 60, QStringLiteral("故障"), 61}};
}

bool MockUserClientService::reserve(int stationId, const QString& chargerCode, QString* userMessage)
{
    if (hasUnfinishedOrder())
    {
        *userMessage = QStringLiteral("您有未完成的充电订单，请先结算");
        return false;
    }
    if (balanceCent_ < 500)
    {
        *userMessage = QStringLiteral("余额不足，请先充值");
        return false;
    }
    const auto chargersAtStation = chargers(stationId);
    const auto selected = std::find_if(chargersAtStation.cbegin(), chargersAtStation.cend(),
                                       [&chargerCode](const ChargerSummary& charger) {
                                           return charger.code == chargerCode &&
                                                  charger.status == QStringLiteral("空闲");
                                       });
    if (selected == chargersAtStation.cend())
    {
        *userMessage = QStringLiteral("请选择一个空闲充电桩");
        return false;
    }
    selectedStationId_ = stationId;
    selectedChargerCode_ = chargerCode;
    reserved_ = true;
    reservationRemainingSeconds_ = 15 * 60;
    activeOrderNo_ = QStringLiteral("OR%1%2")
                         .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMddhhmmss")))
                         .arg(orders_.size() + 1, 4, 10, QLatin1Char('0'));
    orders_.prepend({activeOrderNo_, stationId == 1 ? QStringLiteral("NCS 中关村充电站")
                                                     : QStringLiteral("示范充电站"),
                     chargerCode, {}, {}, 0, 0, 0, QStringLiteral("已预约")});
    *userMessage = QStringLiteral("已为你保留 %1，15 分钟内可开始充电").arg(chargerCode);
    return true;
}

bool MockUserClientService::cancelReservation(QString* userMessage)
{
    if (!reserved_ || charging_)
    {
        *userMessage = QStringLiteral("当前没有可取消的预约");
        return false;
    }
    reserved_ = false;
    reservationRemainingSeconds_ = 0;
    selectedChargerCode_.clear();
    for (OrderSummary& order : orders_)
    {
        if (order.orderNo == activeOrderNo_) order.status = QStringLiteral("已取消");
    }
    activeOrderNo_.clear();
    *userMessage = QStringLiteral("预约已取消，你可以重新选择空闲桩");
    return true;
}

int MockUserClientService::reservationRemainingSeconds() const
{
    return reservationRemainingSeconds_;
}

bool MockUserClientService::hasUnfinishedOrder() const
{
    return reserved_ || charging_;
}

bool MockUserClientService::start(QString* userMessage)
{
    if (!reserved_)
    {
        *userMessage = QStringLiteral("请先选择一个空闲充电桩");
        return false;
    }
    if (reservationRemainingSeconds_ <= 0)
    {
        *userMessage = QStringLiteral("预约已超时，请重新选择空闲充电桩");
        expireReservation();
        return false;
    }
    charging_ = true;
    for (OrderSummary& order : orders_)
    {
        if (order.orderNo == activeOrderNo_)
        {
            order.status = QStringLiteral("充电中");
            order.startTime = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        }
    }
    *userMessage = QStringLiteral("充电已开始，数据每秒刷新");
    return true;
}

void MockUserClientService::tick()
{
    if (reserved_ && !charging_ && reservationRemainingSeconds_ > 0)
    {
        --reservationRemainingSeconds_;
        if (reservationRemainingSeconds_ == 0) expireReservation();
    }
    if (charging_)
    {
        ++elapsedSeconds_;
    }
}

void MockUserClientService::expireReservation()
{
    reserved_ = false;
    reservationRemainingSeconds_ = 0;
    for (OrderSummary& order : orders_)
    {
        if (order.orderNo == activeOrderNo_) order.status = QStringLiteral("已超时");
    }
    selectedChargerCode_.clear();
    activeOrderNo_.clear();
}

ChargeProgress MockUserClientService::progress() const
{
    const int simulatedSeconds = elapsedSeconds_ * 60;
    const int energyMwh = simulatedSeconds * 1000;
    return {simulatedSeconds, energyMwh, energyMwh * 140 / 1000000, 60,
            qMin(95, 28 + elapsedSeconds_)};
}

bool MockUserClientService::settle(QString* userMessage)
{
    if (!charging_)
    {
        *userMessage = QStringLiteral("当前没有进行中的充电订单");
        return false;
    }
    const ChargeProgress current = progress();
    balanceCent_ = qMax(0, balanceCent_ - current.amountCent);
    reserved_ = false;
    charging_ = false;
    reservationRemainingSeconds_ = 0;
    elapsedSeconds_ = 0;
    for (OrderSummary& order : orders_)
    {
        if (order.orderNo == activeOrderNo_)
        {
            order.status = QStringLiteral("已完成");
            order.endTime = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
            order.energyMwh = current.energyMwh;
            order.amountCent = current.amountCent;
            order.durationSeconds = current.durationSeconds;
        }
    }
    activeOrderNo_.clear();
    *userMessage = QStringLiteral("结算成功，充电桩已恢复空闲");
    return true;
}

bool MockUserClientService::recharge(int amountCent, QString* userMessage)
{
    if (amountCent < 1 || amountCent > 1000000)
    {
        *userMessage = QStringLiteral("充值金额应为 0.01 至 10000.00 元");
        return false;
    }
    balanceCent_ += amountCent;
    *userMessage = QStringLiteral("充值成功，余额已更新");
    return true;
}

} // namespace ncs::user
