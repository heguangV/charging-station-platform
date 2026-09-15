#pragma once

#include "infrastructure/postgres/postgres_repository_detail.h"

namespace ncs::infrastructure::postgres::detail
{

using namespace ncs::core::application;

// ---- 行映射辅助：把查询结果的固定列序搬运到领域结构体 ----
// userSelect 联结 user_account/user_credential/user_avatar 三表，列序由
// readUser() 的下标一一对应；流程与订单同样使用共享列清单 + 专用读函数。
inline UserAccount readUser(Statement& statement)
{
    UserAccount account;
    account.id = statement.integer(0);
    account.username = statement.text(1);
    account.phone = statement.text(2);
    account.nickname = statement.text(3);
    account.status = static_cast<int>(statement.integer(4));
    account.registeredAt = statement.integer(5);
    account.balanceCent = statement.integer(6);
    account.debtCent = statement.integer(7);
    account.hasActiveFlow = statement.integer(8) != 0;
    account.version = statement.integer(9);
    account.deleted = statement.integer(10) != 0;
    account.passwordHash = optionalText(statement, 11);
    if (!statement.isNull(12))
    {
        account.avatar = AvatarData{statement.blob(12), statement.text(13), statement.text(14)};
    }
    return account;
}

constexpr const char* userSelect =
    "SELECT u.id,u.username,u.phone,u.nickname,u.status,u.registered_at,"
    "u.balance_cent,u.debt_cent,u.has_active_flow,u.version,u.deleted,"
    "c.password_hash,a.data,a.content_type,a.etag "
    "FROM user_account u LEFT JOIN user_credential c ON c.user_id=u.id "
    "LEFT JOIN user_avatar a ON a.user_id=u.id ";

inline Station readStation(Statement& statement)
{
    Station value;
    value.id = statement.integer(0);
    value.code = statement.text(1);
    value.name = statement.text(2);
    value.address = statement.text(3);
    value.adcode = statement.text(4);
    value.latitudeE6 = statement.integer(5);
    value.longitudeE6 = statement.integer(6);
    value.businessHours = statement.text(7);
    value.enabled = statement.integer(8) != 0;
    value.version = statement.integer(9);
    return value;
}

inline Charger readCharger(Statement& statement)
{
    Charger value;
    value.id = statement.integer(0);
    value.stationId = statement.integer(1);
    value.code = statement.text(2);
    value.type = static_cast<ChargerType>(statement.integer(3));
    value.powerWatt = statement.integer(4);
    value.connectorStandard = statement.text(5);
    value.status = static_cast<ChargerStatus>(statement.integer(6));
    value.totalCount = statement.integer(7);
    value.totalMinutes = statement.integer(8);
    value.version = statement.integer(9);
    return value;
}

constexpr const char* flowColumns =
    "flow_no,user_id,station_id,charger_type,charger_id,charger_code,status,"
    "quote_no,quote_charger_id,quote_charger_code,electricity_price,base_"
    "service_price,"
    "queue_adjustment_bp,ml_adjustment_bp,final_service_price,total_price,"
    "quote_expires_at,"
    "reserved_until,started_at,version,created_at";

inline ChargingFlow readFlow(Statement& statement)
{
    ChargingFlow value;
    value.flowNo = statement.text(0);
    value.userId = statement.integer(1);
    value.stationId = statement.integer(2);
    value.chargerType = static_cast<ChargerType>(statement.integer(3));
    value.chargerId = optionalInteger(statement, 4);
    value.chargerCode = optionalText(statement, 5);
    value.status = static_cast<int>(statement.integer(6));
    if (!statement.isNull(7))
    {
        FlowQuote quote;
        quote.quoteNo = statement.text(7);
        quote.chargerId = statement.integer(8);
        quote.chargerCode = statement.text(9);
        quote.electricityPriceCentPerKwh = static_cast<int>(statement.integer(10));
        quote.baseServicePriceCentPerKwh = static_cast<int>(statement.integer(11));
        quote.queueAdjustmentBp = static_cast<int>(statement.integer(12));
        quote.mlAdjustmentBp = static_cast<int>(statement.integer(13));
        quote.finalServicePriceCentPerKwh = static_cast<int>(statement.integer(14));
        quote.totalPriceCentPerKwh = static_cast<int>(statement.integer(15));
        quote.expiresAt = statement.integer(16);
        value.quote = std::move(quote);
    }
    value.reservedUntil = optionalInteger(statement, 17);
    value.startedAt = optionalInteger(statement, 18);
    value.version = statement.integer(19);
    value.createdAt = statement.integer(20);
    return value;
}

inline void bindFlow(Statement& statement, const ChargingFlow& flow)
{
    statement.bind(1, flow.flowNo);
    statement.bind(2, flow.userId);
    statement.bind(3, flow.stationId);
    statement.bind(4, static_cast<int>(flow.chargerType));
    bindOptional(statement, 5, flow.chargerId);
    bindOptional(statement, 6, flow.chargerCode);
    statement.bind(7, flow.status);
    if (flow.quote)
    {
        statement.bind(8, flow.quote->quoteNo);
        statement.bind(9, flow.quote->chargerId);
        statement.bind(10, flow.quote->chargerCode);
        statement.bind(11, flow.quote->electricityPriceCentPerKwh);
        statement.bind(12, flow.quote->baseServicePriceCentPerKwh);
        statement.bind(13, flow.quote->queueAdjustmentBp);
        statement.bind(14, flow.quote->mlAdjustmentBp);
        statement.bind(15, flow.quote->finalServicePriceCentPerKwh);
        statement.bind(16, flow.quote->totalPriceCentPerKwh);
        statement.bind(17, flow.quote->expiresAt);
    }
    else
    {
        for (int index = 8; index <= 17; ++index)
            statement.bindNull(index);
    }
    bindOptional(statement, 18, flow.reservedUntil);
    bindOptional(statement, 19, flow.startedAt);
    statement.bind(20, flow.version);
    statement.bind(21, flow.createdAt);
}

inline void bindFlowFilters(Statement& statement, const AdminFlowQuery& query)
{
    if (query.status)
        statement.bind(1, static_cast<std::int64_t>(*query.status));
    else
        statement.bindNull(1);
    if (query.status)
        statement.bind(2, static_cast<std::int64_t>(*query.status));
    else
        statement.bindNull(2);
    if (query.stationId)
        statement.bind(3, *query.stationId);
    else
        statement.bindNull(3);
    if (query.stationId)
        statement.bind(4, *query.stationId);
    else
        statement.bindNull(4);
    if (query.chargerId)
        statement.bind(5, *query.chargerId);
    else
        statement.bindNull(5);
    if (query.chargerId)
        statement.bind(6, *query.chargerId);
    else
        statement.bindNull(6);
    if (query.userId)
        statement.bind(7, *query.userId);
    else
        statement.bindNull(7);
    if (query.userId)
        statement.bind(8, *query.userId);
    else
        statement.bindNull(8);
}

inline void bindManagedUserFilters(Statement& statement, const AdminUserQuery& query)
{
    if (query.status)
        statement.bind(1, static_cast<std::int64_t>(*query.status));
    else
        statement.bindNull(1);
    if (query.status)
        statement.bind(2, static_cast<std::int64_t>(*query.status));
    else
        statement.bindNull(2);
    statement.bind(3, query.phoneExact.value_or(""));
    statement.bind(4, query.phoneExact.value_or(""));
    statement.bind(5, query.phoneLast4.value_or(""));
    statement.bind(6, query.phoneLast4.value_or(""));
}

constexpr const char* orderColumns =
    "order_no,flow_no,user_id,station_id,station_name,charger_id,charger_code,"
    "charger_type,"
    "electricity_price,service_price,power_watt,time_scale,target_amount_cent,"
    "status,created_at,"
    "started_at,ended_at,energy_mwh,amount_cent,paid_cent,debt_added_cent,"
    "balance_after_cent,"
    "debt_after_cent,settled_at,appeal_reason,appeal_at,reviewed_by,reviewed_at";

inline ChargingOrder readOrder(Statement& statement)
{
    ChargingOrder value;
    value.orderNo = statement.text(0);
    value.flowNo = statement.text(1);
    value.userId = statement.integer(2);
    value.stationId = statement.integer(3);
    value.stationName = statement.text(4);
    value.chargerId = statement.integer(5);
    value.chargerCode = statement.text(6);
    value.chargerType = static_cast<ChargerType>(statement.integer(7));
    value.electricityPriceCentPerKwh = static_cast<int>(statement.integer(8));
    value.servicePriceCentPerKwh = static_cast<int>(statement.integer(9));
    value.powerWatt = statement.integer(10);
    value.timeScale = static_cast<int>(statement.integer(11));
    value.targetAmountCent = optionalInteger(statement, 12);
    value.status = static_cast<int>(statement.integer(13));
    value.createdAt = statement.integer(14);
    value.startedAt = optionalInteger(statement, 15);
    value.endedAt = optionalInteger(statement, 16);
    value.energyMwh = statement.integer(17);
    value.amountCent = statement.integer(18);
    value.paidCent = statement.integer(19);
    value.debtAddedCent = statement.integer(20);
    value.balanceAfterCent = statement.integer(21);
    value.debtAfterCent = statement.integer(22);
    value.settledAt = optionalInteger(statement, 23);
    value.appealReason = statement.text(24);
    value.appealAt = optionalInteger(statement, 25);
    value.reviewedBy = optionalInteger(statement, 26);
    value.reviewedAt = optionalInteger(statement, 27);
    return value;
}

inline void bindOrder(Statement& statement, const ChargingOrder& order)
{
    statement.bind(1, order.orderNo);
    statement.bind(2, order.flowNo);
    statement.bind(3, order.userId);
    statement.bind(4, order.stationId);
    statement.bind(5, order.stationName);
    statement.bind(6, order.chargerId);
    statement.bind(7, order.chargerCode);
    statement.bind(8, static_cast<int>(order.chargerType));
    statement.bind(9, order.electricityPriceCentPerKwh);
    statement.bind(10, order.servicePriceCentPerKwh);
    statement.bind(11, order.powerWatt);
    statement.bind(12, order.timeScale);
    bindOptional(statement, 13, order.targetAmountCent);
    statement.bind(14, order.status);
    statement.bind(15, order.createdAt);
    bindOptional(statement, 16, order.startedAt);
    bindOptional(statement, 17, order.endedAt);
    statement.bind(18, order.energyMwh);
    statement.bind(19, order.amountCent);
    statement.bind(20, order.paidCent);
    statement.bind(21, order.debtAddedCent);
    statement.bind(22, order.balanceAfterCent);
    statement.bind(23, order.debtAfterCent);
    bindOptional(statement, 24, order.settledAt);
    statement.bind(25, order.appealReason);
    bindOptional(statement, 26, order.appealAt);
    bindOptional(statement, 27, order.reviewedBy);
    bindOptional(statement, 28, order.reviewedAt);
}

inline std::vector<Role> readAdminRoles(QSqlDatabase* database, const std::int64_t adminId)
{
    Statement query(database, "SELECT role FROM admin_role WHERE admin_id=? ORDER BY role");
    query.bind(1, adminId);
    std::vector<Role> roles;
    while (query.row())
    {
        const std::string role = query.text(0);
        if (role == "OWNER")
            roles.push_back(Role::Owner);
        else if (role == "OPERATOR")
            roles.push_back(Role::Operator);
        else if (role == "VIEWER")
            roles.push_back(Role::Viewer);
    }
    return roles;
}

inline AdminAccount readAdmin(QSqlDatabase* database, Statement& statement)
{
    AdminAccount account;
    account.id = statement.integer(0);
    account.username = statement.text(1);
    account.passwordHash = statement.text(2);
    account.status = static_cast<int>(statement.integer(3));
    account.mustChangePassword = statement.integer(4) != 0;
    account.version = statement.integer(5);
    account.roles = readAdminRoles(database, account.id);
    return account;
}

inline std::string encodeHorizons(const std::vector<int>& horizons)
{
    std::string encoded;
    for (const int horizon : horizons)
    {
        if (!encoded.empty())
            encoded.push_back(',');
        encoded += std::to_string(horizon);
    }
    return encoded;
}

inline std::vector<int> decodeHorizons(const std::string_view encoded)
{
    std::vector<int> result;
    std::size_t start = 0;
    while (start < encoded.size())
    {
        const std::size_t comma = encoded.find(',', start);
        const std::string_view item = encoded.substr(
            start, comma == std::string_view::npos ? encoded.size() - start : comma - start);
        int value = 0;
        const auto parsed = std::from_chars(item.data(), item.data() + item.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != item.data() + item.size())
            return {};
        result.push_back(value);
        if (comma == std::string_view::npos)
            break;
        start = comma + 1;
    }
    return result;
}

inline MlTask readMlTask(Statement& statement)
{
    MlTask task;
    task.taskNo = statement.text(0);
    task.taskType = statement.text(1);
    task.status = statement.text(2);
    task.modelVersion = statement.text(3);
    task.horizonHours = decodeHorizons(statement.text(4));
    task.createdAt = statement.integer(5);
    task.finishedAt = optionalInteger(statement, 6);
    task.metricsSummary = statement.text(7);
    task.errorSummary = statement.text(8);
    return task;
}

} // namespace ncs::infrastructure::postgres::detail
