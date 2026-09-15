#include "infrastructure/postgres/postgres_repository.h"
#include "infrastructure/postgres/postgres_row_mappers.h"

namespace ncs::infrastructure::postgres
{

using namespace ncs::core::application;
using namespace detail;

// ---- 订单域：业务凭证（价格/功率/倍率快照 + 结算结余字段）----
// 历史计费以本表快照为准，费率后续变化不影响已有订单。
void PostgresRepository::addOrder(const ChargingOrder& value)
{
    useDatabase(this, config_,
                [&](QSqlDatabase* database)
                {
                    const std::string sql =
                        std::string("INSERT INTO charging_order(") + orderColumns +
                        ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
                    Statement insert(database, sql.c_str());
                    bindOrder(insert, value);
                    insert.execute();
                });
}

std::optional<OrderReview> PostgresRepository::orderReview(const std::string& orderNo)
{
    return useDatabase(
        this, config_,
        [&](QSqlDatabase* database) -> std::optional<OrderReview>
        {
            Statement query(database, "SELECT order_no,user_id,rating,content,created_at FROM "
                                      "order_review WHERE order_no=?");
            query.bind(1, orderNo);
            if (!query.row())
                return std::nullopt;
            return OrderReview{query.text(0), query.integer(1), static_cast<int>(query.integer(2)),
                               query.text(3), query.integer(4)};
        });
}

void PostgresRepository::addOrderReview(const OrderReview& review)
{
    useDatabase(this, config_,
                [&](QSqlDatabase* database)
                {
                    Statement insert(
                        database,
                        "INSERT INTO order_review(order_no,user_id,rating,content,created_at) "
                        "VALUES(?,?,?,?,?)");
                    insert.bind(1, review.orderNo);
                    insert.bind(2, review.userId);
                    insert.bind(3, review.rating);
                    insert.bind(4, review.content);
                    insert.bind(5, review.createdAt);
                    insert.execute();
                });
}

std::vector<StationReviewRow> PostgresRepository::stationReviewRows(const std::int64_t stationId,
                                                                    const std::size_t limit)
{
    return useDatabase(this, config_,
                       [&](QSqlDatabase* database) -> std::vector<StationReviewRow>
                       {
                           // 评论墙视图：评价经订单归属到场站，联账号表取展示名；脱敏在应用服务完成。
                           Statement query(
                               database, "SELECT r.user_id,u.nickname,u.phone,r.rating,r.content,"
                                         "r.created_at FROM order_review r "
                                         "JOIN charging_order o ON o.order_no=r.order_no "
                                         "LEFT JOIN user_account u ON u.id=r.user_id "
                                         "WHERE o.station_id=? ORDER BY r.created_at DESC LIMIT ?");
                           query.bind(1, stationId);
                           query.bind(2, static_cast<std::int64_t>(limit));
                           std::vector<StationReviewRow> rows;
                           while (query.row())
                               rows.push_back({query.integer(0), query.text(1), query.text(2),
                                               static_cast<int>(query.integer(3)), query.text(4),
                                               query.integer(5)});
                           return rows;
                       });
}

void PostgresRepository::saveOrder(const ChargingOrder& value)
{
    useDatabase(this, config_,
                [&](QSqlDatabase* database)
                {
                    Statement update(
                        database,
                        "UPDATE charging_order SET "
                        "flow_no=?,user_id=?,station_id=?,station_name=?,charger_id=?,charger_"
                        "code=?,charger_type=?,electricity_price=?,service_price=?,power_watt=?"
                        ",time_scale=?,target_amount_cent=?,status=?,created_at=?,started_at=?,"
                        "ended_at=?,energy_mwh=?,amount_cent=?,paid_cent=?,debt_added_cent=?,"
                        "balance_after_cent=?,debt_after_cent=?,settled_at=?,appeal_reason=?,"
                        "appeal_at=?,reviewed_by=?,reviewed_at=? WHERE order_no=?");
                    update.bind(1, value.flowNo);
                    update.bind(2, value.userId);
                    update.bind(3, value.stationId);
                    update.bind(4, value.stationName);
                    update.bind(5, value.chargerId);
                    update.bind(6, value.chargerCode);
                    update.bind(7, static_cast<int>(value.chargerType));
                    update.bind(8, value.electricityPriceCentPerKwh);
                    update.bind(9, value.servicePriceCentPerKwh);
                    update.bind(10, value.powerWatt);
                    update.bind(11, value.timeScale);
                    bindOptional(update, 12, value.targetAmountCent);
                    update.bind(13, value.status);
                    update.bind(14, value.createdAt);
                    bindOptional(update, 15, value.startedAt);
                    bindOptional(update, 16, value.endedAt);
                    update.bind(17, value.energyMwh);
                    update.bind(18, value.amountCent);
                    update.bind(19, value.paidCent);
                    update.bind(20, value.debtAddedCent);
                    update.bind(21, value.balanceAfterCent);
                    update.bind(22, value.debtAfterCent);
                    bindOptional(update, 23, value.settledAt);
                    update.bind(24, value.appealReason);
                    bindOptional(update, 25, value.appealAt);
                    bindOptional(update, 26, value.reviewedBy);
                    bindOptional(update, 27, value.reviewedAt);
                    update.bind(28, value.orderNo);
                    update.execute();
                });
}

std::optional<ChargingOrder> PostgresRepository::order(const std::string& orderNo)
{
    return useDatabase(this, config_,
                       [&](QSqlDatabase* database) -> std::optional<ChargingOrder>
                       {
                           const std::string sql = std::string("SELECT ") + orderColumns +
                                                   " FROM charging_order WHERE order_no=?" +
                                                   forUpdate(this);
                           Statement query(database, sql.c_str());
                           query.bind(1, orderNo);
                           return query.row() ? std::optional<ChargingOrder>(readOrder(query))
                                              : std::nullopt;
                       });
}

std::optional<ChargingOrder> PostgresRepository::orderByFlow(const std::string& flowNo)
{
    return useDatabase(this, config_,
                       [&](QSqlDatabase* database) -> std::optional<ChargingOrder>
                       {
                           const std::string sql = std::string("SELECT ") + orderColumns +
                                                   " FROM charging_order WHERE flow_no=?" +
                                                   forUpdate(this);
                           Statement query(database, sql.c_str());
                           query.bind(1, flowNo);
                           return query.row() ? std::optional<ChargingOrder>(readOrder(query))
                                              : std::nullopt;
                       });
}

std::vector<ChargingOrder> PostgresRepository::orders(const std::int64_t userId,
                                                      const std::optional<int> status,
                                                      const std::int64_t fromAt,
                                                      const std::int64_t toAt)
{
    return useDatabase(this, config_,
                       [&](QSqlDatabase* database)
                       {
                           const std::string sql =
                               std::string("SELECT ") + orderColumns +
                               " FROM charging_order WHERE user_id=? AND (CAST(? AS INTEGER) IS "
                               "NULL OR status=?) AND "
                               "created_at>=? AND (?=0 OR created_at<=?) ORDER BY created_at "
                               "DESC,order_no DESC";
                           Statement query(database, sql.c_str());
                           query.bind(1, userId);
                           if (status)
                               query.bind(2, *status);
                           else
                               query.bindNull(2);
                           if (status)
                               query.bind(3, *status);
                           else
                               query.bindNull(3);
                           query.bind(4, fromAt);
                           query.bind(5, toAt);
                           query.bind(6, toAt);
                           std::vector<ChargingOrder> values;
                           while (query.row())
                               values.push_back(readOrder(query));
                           return values;
                       });
}

std::vector<UserAccount> PostgresRepository::listAccounts()
{
    return useDatabase(this, config_,
                       [&](QSqlDatabase* database)
                       {
                           Statement query(database, (std::string(userSelect) + "WHERE u.deleted=0 "
                                                                                "ORDER BY u.id")
                                                         .c_str());
                           std::vector<UserAccount> values;
                           while (query.row())
                               values.push_back(readUser(query));
                           return values;
                       });
}

// 新建站点：数据库 identity 原子分配 id，唯一索引兜底编码竞争；外层服务编排事务。
bool PostgresRepository::addStation(Station& station)
{
    return useDatabase(this, config_,
                       [&](QSqlDatabase* database) -> bool
                       {
                           Statement exists(database, "SELECT 1 FROM station WHERE code=? LIMIT 1");
                           exists.bind(1, station.code);
                           if (exists.row())
                               return false;
                           Statement insert(database,
                                            "INSERT INTO "
                                            "station(code,name,address,adcode,latitude_e6,"
                                            "longitude_e6,business_hours,enabled,version) "
                                            "VALUES(?,?,?,?,?,?,?,?,?) RETURNING id");
                           insert.bind(1, station.code);
                           insert.bind(2, station.name);
                           insert.bind(3, station.address);
                           insert.bind(4, station.adcode);
                           insert.bind(5, station.latitudeE6);
                           insert.bind(6, station.longitudeE6);
                           insert.bind(7, station.businessHours);
                           insert.bind(8, station.enabled ? 1 : 0);
                           insert.bind(9, station.version);
                           if (!insert.row())
                               throw std::runtime_error("station insert did not return an id");
                           station.id = insert.integer(0);
                           return true;
                       });
}

// 站点更新（乐观锁）：WHERE 同时匹配 id 与 version-1，0 行受影响即并发冲突。
bool PostgresRepository::saveStation(const Station& station)
{
    return useDatabase(this, config_,
                       [&](QSqlDatabase* database) -> bool
                       {
                           Statement update(database,
                                            "UPDATE station SET code=?,name=?,address=?,adcode=?,"
                                            "latitude_e6=?,longitude_e6=?,business_hours=?,"
                                            "enabled=?,version=? WHERE id=? AND version=?");
                           update.bind(1, station.code);
                           update.bind(2, station.name);
                           update.bind(3, station.address);
                           update.bind(4, station.adcode);
                           update.bind(5, station.latitudeE6);
                           update.bind(6, station.longitudeE6);
                           update.bind(7, station.businessHours);
                           update.bind(8, station.enabled ? 1 : 0);
                           update.bind(9, station.version);
                           update.bind(10, station.id);
                           update.bind(11, station.version - 1);
                           update.execute();
                           return update.rowsAffected() > 0;
                       });
}

bool PostgresRepository::stationCodeExists(const std::string& code)
{
    return useDatabase(this, config_,
                       [&](QSqlDatabase* database) -> bool
                       {
                           Statement query(database, "SELECT 1 FROM station WHERE code=? LIMIT 1");
                           query.bind(1, code);
                           return query.row();
                       });
}

// 新建设备：数据库 identity 原子分配 id，唯一索引兜底编码竞争。
bool PostgresRepository::addCharger(Charger& charger)
{
    return useDatabase(this, config_,
                       [&](QSqlDatabase* database) -> bool
                       {
                           Statement exists(database, "SELECT 1 FROM charger WHERE code=? LIMIT 1");
                           exists.bind(1, charger.code);
                           if (exists.row())
                               return false;
                           Statement insert(
                               database,
                               "INSERT INTO "
                               "charger(station_id,code,charger_type,power_watt,"
                               "connector_standard,status,total_count,total_minutes,version)"
                               " VALUES(?,?,?,?,?,?,?,?,?) RETURNING id");
                           insert.bind(1, charger.stationId);
                           insert.bind(2, charger.code);
                           insert.bind(3, static_cast<int>(charger.type));
                           insert.bind(4, charger.powerWatt);
                           insert.bind(5, charger.connectorStandard);
                           insert.bind(6, static_cast<int>(charger.status));
                           insert.bind(7, charger.totalCount);
                           insert.bind(8, charger.totalMinutes);
                           insert.bind(9, charger.version);
                           if (!insert.row())
                               throw std::runtime_error("charger insert did not return an id");
                           charger.id = insert.integer(0);
                           return true;
                       });
}

bool PostgresRepository::chargerCodeExists(const std::string& code)
{
    return useDatabase(this, config_,
                       [&](QSqlDatabase* database) -> bool
                       {
                           Statement query(database, "SELECT 1 FROM charger WHERE code=? LIMIT 1");
                           query.bind(1, code);
                           return query.row();
                       });
}

void PostgresRepository::addTariff(const RegionTariff& tariff)
{
    useDatabase(this, config_,
                [&](QSqlDatabase* database)
                {
                    Statement insert(
                        database,
                        "INSERT INTO "
                        "region_tariff(adcode,electricity_cent_per_kwh,service_cent_per_kwh,"
                        "effective_from,effective_to) VALUES(?,?,?,?,?)");
                    insert.bind(1, tariff.adcode);
                    insert.bind(2, tariff.electricityCentPerKwh);
                    insert.bind(3, tariff.serviceCentPerKwh);
                    insert.bind(4, tariff.effectiveFrom);
                    insert.bind(5, tariff.effectiveTo);
                    insert.execute();
                });
}

std::vector<RegionTariff> PostgresRepository::tariffVersions(std::optional<std::string> adcode)
{
    return useDatabase(
        this, config_,
        [&](QSqlDatabase* database)
        {
            Statement query(database,
                            "SELECT "
                            "adcode,electricity_cent_per_kwh,service_cent_per_kwh,"
                            "effective_from,effective_to FROM region_tariff WHERE "
                            "(CAST(? AS TEXT) IS NULL OR adcode=?) ORDER BY effective_from");
            if (adcode)
                query.bind(1, *adcode);
            else
                query.bindNull(1);
            if (adcode)
                query.bind(2, *adcode);
            else
                query.bindNull(2);
            std::vector<RegionTariff> values;
            while (query.row())
                values.push_back(RegionTariff{query.text(0), static_cast<int>(query.integer(1)),
                                              static_cast<int>(query.integer(2)), query.integer(3),
                                              query.integer(4)});
            return values;
        });
}

std::vector<ChargingFlow> PostgresRepository::allFlows()
{
    return useDatabase(this, config_,
                       [&](QSqlDatabase* database)
                       {
                           const std::string sql = std::string("SELECT ") + flowColumns +
                                                   " FROM charging_flow ORDER BY created_at";
                           Statement query(database, sql.c_str());
                           std::vector<ChargingFlow> values;
                           while (query.row())
                               values.push_back(readFlow(query));
                           return values;
                       });
}

std::vector<ChargingOrder> PostgresRepository::allOrders()
{
    return useDatabase(this, config_,
                       [&](QSqlDatabase* database)
                       {
                           const std::string sql =
                               std::string("SELECT ") + orderColumns +
                               " FROM charging_order ORDER BY created_at DESC,order_no DESC";
                           Statement query(database, sql.c_str());
                           std::vector<ChargingOrder> values;
                           while (query.row())
                               values.push_back(readOrder(query));
                           return values;
                       });
}

} // namespace ncs::infrastructure::postgres
