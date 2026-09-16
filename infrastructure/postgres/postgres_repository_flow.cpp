#include "infrastructure/postgres/postgres_repository.h"
#include "infrastructure/postgres/postgres_row_mappers.h"

namespace ncs::infrastructure::postgres
{

using namespace ncs::core::application;
using namespace detail;

// ---- 充电流程域：流程状态机 + 状态事件 + Outbox + 排队 ----
// addFlowEvent 同事务写 flow_event（证据）与 outbox_event（投递），
// 投递失败按指数退避重试、10 次转死信（markOutboxAttempted）。
void PostgresRepository::addFlow(const ChargingFlow& value)
{
    useDatabase(this, config_,
                [&](QSqlDatabase* database)
                {
                    const std::string sql = std::string("INSERT INTO charging_flow(") +
                                            flowColumns +
                                            ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
                    Statement insert(database, sql.c_str());
                    bindFlow(insert, value);
                    insert.execute();
                });
}

void PostgresRepository::saveFlow(const ChargingFlow& value)
{
    useDatabase(this, config_,
                [&](QSqlDatabase* database)
                {
                    Statement update(database,
                                     "UPDATE charging_flow SET "
                                     "user_id=?,station_id=?,charger_type=?,charger_id=?,charger_"
                                     "code=?,status=?,quote_no=?,quote_charger_id=?,quote_charger_"
                                     "code=?,electricity_price=?,base_service_price=?,queue_"
                                     "adjustment_bp=?,ml_adjustment_bp=?,final_service_price=?,"
                                     "total_price=?,quote_expires_at=?,reserved_until=?,started_"
                                     "at=?,version=?,created_at=? WHERE flow_no=?");
                    update.bind(1, value.userId);
                    update.bind(2, value.stationId);
                    update.bind(3, static_cast<int>(value.chargerType));
                    bindOptional(update, 4, value.chargerId);
                    bindOptional(update, 5, value.chargerCode);
                    update.bind(6, value.status);
                    if (value.quote)
                    {
                        update.bind(7, value.quote->quoteNo);
                        update.bind(8, value.quote->chargerId);
                        update.bind(9, value.quote->chargerCode);
                        update.bind(10, value.quote->electricityPriceCentPerKwh);
                        update.bind(11, value.quote->baseServicePriceCentPerKwh);
                        update.bind(12, value.quote->queueAdjustmentBp);
                        update.bind(13, value.quote->mlAdjustmentBp);
                        update.bind(14, value.quote->finalServicePriceCentPerKwh);
                        update.bind(15, value.quote->totalPriceCentPerKwh);
                        update.bind(16, value.quote->expiresAt);
                    }
                    else
                    {
                        for (int index = 7; index <= 16; ++index)
                            update.bindNull(index);
                    }
                    bindOptional(update, 17, value.reservedUntil);
                    bindOptional(update, 18, value.startedAt);
                    update.bind(19, value.version);
                    update.bind(20, value.createdAt);
                    update.bind(21, value.flowNo);
                    update.execute();
                });
}

std::optional<ChargingFlow> PostgresRepository::flow(const std::string& flowNo)
{
    return useDatabase(this, config_,
                       [&](QSqlDatabase* database) -> std::optional<ChargingFlow>
                       {
                           const std::string sql = std::string("SELECT ") + flowColumns +
                                                   " FROM charging_flow WHERE flow_no=?" +
                                                   forUpdate(this);
                           Statement query(database, sql.c_str());
                           query.bind(1, flowNo);
                           return query.row() ? std::optional<ChargingFlow>(readFlow(query))
                                              : std::nullopt;
                       });
}

// 活动流程查询：状态集合固定为 10/20/30/40/50/80（未完成+待恢复），
// 与部分唯一索引、注销拦截、/flows/active 使用同一口径。
std::optional<ChargingFlow> PostgresRepository::activeFlow(const std::int64_t userId)
{
    return useDatabase(
        this, config_,
        [&](QSqlDatabase* database) -> std::optional<ChargingFlow>
        {
            const std::string sql = std::string("SELECT ") + flowColumns +
                                    " FROM charging_flow WHERE user_id=? AND status IN "
                                    "(10,20,30,40,50,80) ORDER BY created_at DESC LIMIT 1" +
                                    forUpdate(this);
            Statement query(database, sql.c_str());
            query.bind(1, userId);
            return query.row() ? std::optional<ChargingFlow>(readFlow(query)) : std::nullopt;
        });
}

std::vector<ChargingFlow> PostgresRepository::flowsWithStatus(const int status)
{
    return useDatabase(this, config_,
                       [&](QSqlDatabase* database)
                       {
                           const std::string sql =
                               std::string("SELECT ") + flowColumns +
                               " FROM charging_flow WHERE status=? ORDER BY created_at";
                           Statement query(database, sql.c_str());
                           query.bind(1, status);
                           std::vector<ChargingFlow> values;
                           while (query.row())
                               values.push_back(readFlow(query));
                           return values;
                       });
}

void PostgresRepository::addFlowEvent(const FlowEvent& value)
{
    useDatabase(
        this, config_,
        [&](QSqlDatabase* database)
        {
            Statement insert(database,
                             "INSERT INTO flow_event(flow_no,from_status,to_status,reason_code,at) "
                             "VALUES(?,?,?,?,?)");
            insert.bind(1, value.flowNo);
            insert.bind(2, value.fromStatus);
            insert.bind(3, value.toStatus);
            insert.bind(4, value.reasonCode);
            insert.bind(5, value.at);
            insert.execute();

            Statement outbox(database,
                             "INSERT INTO outbox_event(event_type,aggregate_type,aggregate_id,"
                             "from_status,to_status,reason_code,created_at,available_at) "
                             "VALUES(?,?,?,?,?,?,?,?)");
            // 事件类型必须走 core 的统一映射：结算目标从 60 改为 100 后，
            // 只特判 Completed 会漏掉 order.ready / order.appealed，
            // 投递器按事件类型筛选，漏映射会导致通知被静默丢弃。
            outbox.bind(1, flowEventType(value.toStatus, value.reasonCode));
            outbox.bind(2, std::string_view("charging_flow"));
            outbox.bind(3, value.flowNo);
            outbox.bind(4, value.fromStatus);
            outbox.bind(5, value.toStatus);
            outbox.bind(6, value.reasonCode);
            outbox.bind(7, value.at);
            outbox.bind(8, value.at);
            outbox.execute();
        });
}

void PostgresRepository::addChargerStatusEvent(const ChargerStatusEvent& value)
{
    useDatabase(this, config_,
                [&](QSqlDatabase* database)
                {
                    Statement insert(
                        database, "INSERT INTO outbox_event(event_type,aggregate_type,aggregate_id,"
                                  "from_status,to_status,reason_code,created_at,available_at) "
                                  "VALUES(?,?,?,?,?,?,?,?)");
                    insert.bind(1, std::string_view("charger.statusChanged"));
                    insert.bind(2, std::string_view("charger"));
                    insert.bind(3, std::to_string(value.chargerId));
                    insert.bind(4, value.fromStatus);
                    insert.bind(5, value.toStatus);
                    insert.bind(6, value.reason);
                    insert.bind(7, value.at);
                    insert.bind(8, value.at);
                    insert.execute();
                });
}

// 投递器轮询：取已到期（available_at<=now）的待投递事件，按 id 升序限量返回。
std::vector<OutboxEvent> PostgresRepository::pollOutbox(const std::int64_t now, const int limit)
{
    std::vector<OutboxEvent> result;
    useDatabase(this, config_,
                [&](QSqlDatabase* database)
                {
                    Statement select(
                        database,
                        "SELECT id,event_type,aggregate_type,aggregate_id,from_status,"
                        "to_status,reason_code,created_at,delivery_status,delivery_attempts,"
                        "available_at FROM outbox_event "
                        "WHERE delivery_status=0 AND available_at<=? ORDER BY id LIMIT ?");
                    select.bind(1, now);
                    select.bind(2, limit);
                    while (select.row())
                    {
                        OutboxEvent row;
                        row.id = select.integer(0);
                        row.eventType = select.text(1);
                        row.aggregateType = select.text(2);
                        row.aggregateId = select.text(3);
                        row.fromStatus = static_cast<int>(select.integer(4));
                        row.toStatus = static_cast<int>(select.integer(5));
                        row.reasonCode = select.text(6);
                        row.createdAt = select.integer(7);
                        row.deliveryStatus = static_cast<int>(select.integer(8));
                        row.deliveryAttempts = static_cast<int>(select.integer(9));
                        row.availableAt = select.integer(10);
                        result.push_back(std::move(row));
                    }
                });
    return result;
}

void PostgresRepository::markOutboxDelivered(const std::vector<std::int64_t>& ids)
{
    if (ids.empty())
        return;
    const std::int64_t publishedAt = std::chrono::duration_cast<std::chrono::seconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count();
    useDatabase(this, config_,
                [&](QSqlDatabase* database)
                {
                    for (const auto id : ids)
                    {
                        Statement update(database,
                                         "UPDATE outbox_event SET delivery_status=1,published_at=? "
                                         "WHERE id=? AND delivery_status=0");
                        update.bind(1, publishedAt);
                        update.bind(2, id);
                        update.execute();
                    }
                });
}

// 投递失败记账：attempts+1 并按 5*2^n 秒（封顶 300s）推迟下次投递，
// 累计 10 次在 SQL 内原子转死信（delivery_status=2）。
void PostgresRepository::markOutboxAttempted(const std::vector<std::int64_t>& ids)
{
    if (ids.empty())
        return;
    const std::int64_t nowSeconds = std::chrono::duration_cast<std::chrono::seconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count();
    useDatabase(this, config_,
                [&](QSqlDatabase* database)
                {
                    for (const auto id : ids)
                    {
                        // Backoff: a failed row is not re-polled until 5*2^attempts seconds
                        // (capped at 300) have passed, so a transient failure can no longer
                        // burn all ten attempts within ~20 seconds of ticks and flip the row
                        // to dead prematurely. Ten attempts still dead-letter the row.
                        Statement update(
                            database,
                            "UPDATE outbox_event SET delivery_attempts=delivery_attempts+1,"
                            "available_at=GREATEST(available_at,?+LEAST(300,5*(1<<LEAST(delivery_"
                            "attempts,6)))),"
                            "delivery_status=CASE WHEN delivery_attempts+1>=10 THEN 2 "
                            "ELSE delivery_status END WHERE id=?");
                        update.bind(1, nowSeconds);
                        update.bind(2, id);
                        update.execute();
                    }
                });
}

void PostgresRepository::markOutboxDead(const std::vector<std::int64_t>& ids)
{
    if (ids.empty())
        return;
    useDatabase(this, config_,
                [&](QSqlDatabase* database)
                {
                    for (const auto id : ids)
                    {
                        Statement update(database,
                                         "UPDATE outbox_event SET delivery_status=2 WHERE id=?");
                        update.bind(1, id);
                        update.execute();
                    }
                });
}

void PostgresRepository::enqueue(const std::int64_t stationId, const ChargerType type,
                                 const std::string& flowNo)
{
    useDatabase(this, config_,
                [&](QSqlDatabase* database)
                {
                    Statement insert(database,
                                     "INSERT INTO flow_queue(station_id,charger_type,flow_no) "
                                     "VALUES(?,?,?)");
                    insert.bind(1, stationId);
                    insert.bind(2, static_cast<int>(type));
                    insert.bind(3, flowNo);
                    insert.execute();
                });
}

void PostgresRepository::dequeue(const std::int64_t stationId, const ChargerType type,
                                 const std::string& flowNo)
{
    useDatabase(this, config_,
                [&](QSqlDatabase* database)
                {
                    Statement remove(database, "DELETE FROM flow_queue WHERE station_id=? AND "
                                               "charger_type=? AND flow_no=?");
                    remove.bind(1, stationId);
                    remove.bind(2, static_cast<int>(type));
                    remove.bind(3, flowNo);
                    remove.execute();
                });
}

std::deque<std::string> PostgresRepository::queue(const std::int64_t stationId,
                                                  const ChargerType type)
{
    return useDatabase(this, config_,
                       [&](QSqlDatabase* database)
                       {
                           const std::string sql =
                               "SELECT flow_no FROM flow_queue WHERE station_id=? AND "
                               "charger_type=? ORDER BY sequence" +
                               forUpdate(this, true);
                           Statement query(database, sql.c_str());
                           query.bind(1, stationId);
                           query.bind(2, static_cast<int>(type));
                           std::deque<std::string> values;
                           while (query.row())
                               values.push_back(query.text(0));
                           return values;
                       });
}

} // namespace ncs::infrastructure::postgres
