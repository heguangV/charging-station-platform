#include "infrastructure/postgres/postgres_repository.h"
#include "infrastructure/postgres/postgres_row_mappers.h"

namespace ncs::infrastructure::postgres
{

using namespace ncs::core::application;
using namespace detail;

// ---- 管理员域：演示管理员、账号 CRUD、托管用户、审计（ops_log）----
// 管理员变更全部走乐观锁 version + 变更与审计事件同事务提交。
// 演示管理员（仅开发模式）：enabled=false 时停用所有 is_demo 账号；
// enabled=true 时缺失才创建 admin/123456（OPERATOR+OWNER，is_demo=1）。
void PostgresRepository::ensureDevelopmentAdmin(const bool enabled)
{
    if (!enabled)
    {
        useDatabase(this, config_,
                    [](QSqlDatabase* database)
                    {
                        Statement disable(database, "UPDATE admin_account SET status=0 "
                                                    "WHERE is_demo=1 AND status<>0");
                        disable.execute();
                    });
        return;
    }
    const bool exists = useDatabase(
        this, config_,
        [](QSqlDatabase* database)
        {
            Statement query(database, "SELECT 1 FROM admin_account WHERE username='admin'");
            return query.row();
        });
    if (exists)
        return;
    const std::string passwordHash =
        PasswordHasher().hash("123456", PasswordHasher::currentIterations, 6);
    withTransaction(
        [&]
        {
            Statement insert(transactionContext.database,
                             "INSERT INTO admin_account(username,password_hash,status,"
                             "must_change_password,is_demo,version) VALUES('admin',?,1,0,1,1) "
                             "ON CONFLICT (username) DO NOTHING");
            insert.bind(1, passwordHash);
            insert.execute();
            Statement roles(transactionContext.database,
                            "INSERT INTO admin_role(admin_id,role) "
                            "SELECT id,? FROM admin_account WHERE username='admin' AND is_demo=1 "
                            "ON CONFLICT (admin_id,role) DO NOTHING");
            roles.bind(1, "OPERATOR");
            roles.execute();
            Statement owner(
                transactionContext.database,
                "INSERT INTO admin_role(admin_id,role) "
                "SELECT id,'OWNER' FROM admin_account WHERE username='admin' AND is_demo=1 "
                "ON CONFLICT (admin_id,role) DO NOTHING");
            owner.execute();
        });
}

std::optional<AdminAccount> PostgresRepository::findAdminByUsername(const std::string_view username)
{
    return useDatabase(this, config_,
                       [&](QSqlDatabase* database) -> std::optional<AdminAccount>
                       {
                           Statement query(database,
                                           "SELECT id,username,password_hash,status,"
                                           "must_change_password,version FROM admin_account "
                                           "WHERE username=?");
                           query.bind(1, username);
                           return query.row()
                                      ? std::optional<AdminAccount>(readAdmin(database, query))
                                      : std::nullopt;
                       });
}

std::optional<AdminAccount> PostgresRepository::findAdminById(const std::int64_t id)
{
    return useDatabase(
        this, config_,
        [&](QSqlDatabase* database) -> std::optional<AdminAccount>
        {
            Statement query(database, "SELECT id,username,password_hash,status,"
                                      "must_change_password,version FROM admin_account WHERE id=?");
            query.bind(1, id);
            return query.row() ? std::optional<AdminAccount>(readAdmin(database, query))
                               : std::nullopt;
        });
}

// 托管用户分页：排序仅从代码白名单映射（registeredAt/balanceCent 及其降序）；
// 手机号只支持完整精确匹配或后四位（substr(phone,8)），不提供模糊扫描。
AdminUserPage PostgresRepository::listManagedUsers(const AdminUserQuery& query)
{
    return useDatabase(
        this, config_,
        [&](QSqlDatabase* database)
        {
            const std::string predicate = " FROM user_account WHERE deleted=0 AND (CAST(? AS "
                                          "INTEGER) IS NULL OR status=?) AND "
                                          "(?='' OR phone=?) AND (?='' OR substr(phone,8)=?)";
            Statement count(database, (std::string("SELECT COUNT(*)") + predicate).c_str());
            bindManagedUserFilters(count, query);
            AdminUserPage page;
            page.page = query.page;
            page.pageSize = query.pageSize;
            if (count.row())
                page.total = static_cast<int>(count.integer(0));
            std::string orderBy = "registered_at DESC,id DESC";
            if (query.sort == "registeredAt")
                orderBy = "registered_at ASC,id ASC";
            else if (query.sort == "-registeredAt")
                orderBy = "registered_at DESC,id DESC";
            else if (query.sort == "balanceCent")
                orderBy = "balance_cent ASC,id ASC";
            else if (query.sort == "-balanceCent")
                orderBy = "balance_cent DESC,id DESC";
            Statement select(database,
                             (std::string("SELECT id,username,phone,nickname,status,registered_at,"
                                          "balance_cent,debt_cent,version") +
                              predicate + " ORDER BY " + orderBy + " LIMIT ? OFFSET ?")
                                 .c_str());
            bindManagedUserFilters(select, query);
            select.bind(7, query.pageSize);
            select.bind(8, static_cast<std::int64_t>(query.page - 1) * query.pageSize);
            while (select.row())
            {
                UserAccount account;
                account.id = select.integer(0);
                account.username = select.text(1);
                account.phone = select.text(2);
                account.nickname = select.text(3);
                account.status = static_cast<int>(select.integer(4));
                account.registeredAt = select.integer(5);
                account.balanceCent = select.integer(6);
                account.debtCent = select.integer(7);
                account.version = select.integer(8);
                page.items.push_back(std::move(account));
            }
            return page;
        });
}

std::optional<UserAccount> PostgresRepository::findManagedUser(const std::int64_t id)
{
    return findById(id);
}

// 冻结/解冻（乐观锁）：version 匹配才更新，成功同事务写 USER_FROZEN/UNFROZEN 审计。
AccountWriteResult PostgresRepository::updateManagedUserStatus(
    const std::int64_t actorAdminId, const std::int64_t userId, const int status,
    const std::string_view reason, const std::int64_t expectedVersion, const std::int64_t at,
    UserAccount& updated)
{
    AccountWriteResult result = AccountWriteResult::NotFound;
    withTransaction(
        [&]
        {
            Statement update(transactionContext.database,
                             "UPDATE user_account SET status=?,version=version+1 "
                             "WHERE id=? AND version=? AND deleted=0");
            update.bind(1, status);
            update.bind(2, userId);
            update.bind(3, expectedVersion);
            update.execute();
            if (update.rowsAffected() == 0)
            {
                result = findById(userId) ? AccountWriteResult::VersionConflict
                                          : AccountWriteResult::NotFound;
                return;
            }
            updated = *findById(userId);
            addAuditEvent(AuditEvent{actorAdminId, status == 0 ? "USER_FROZEN" : "USER_UNFROZEN",
                                     "USER", std::to_string(userId), std::string(reason), at});
            result = AccountWriteResult::Success;
        });
    return result;
}

void PostgresRepository::addAuditEvent(const AuditEvent& event)
{
    useDatabase(this, config_,
                [&](QSqlDatabase* database)
                {
                    Statement insert(database,
                                     "INSERT INTO ops_log(actor_admin_id,action,target_type,"
                                     "target_id,reason,at) VALUES(?,?,?,?,?,?)");
                    insert.bind(1, event.actorAdminId);
                    insert.bind(2, event.action);
                    insert.bind(3, event.targetType);
                    insert.bind(4, event.targetId);
                    insert.bind(5, event.reason);
                    insert.bind(6, event.at);
                    insert.execute();
                });
}

std::vector<AuditEvent> PostgresRepository::auditEvents(const AuditEventQuery& query)
{
    return useDatabase(
        this, config_,
        [&](QSqlDatabase* database)
        {
            Statement select(
                database,
                "SELECT actor_admin_id,action,target_type,target_id,reason,at FROM ops_log "
                "WHERE (?='' OR CAST(actor_admin_id AS TEXT)=?) AND (?='' OR action=?) "
                "AND (?='' OR target_type=?) AND (?='' OR target_id=?) "
                "AND (?<=0 OR at>=?) AND (?<=0 OR at<=?) ORDER BY at DESC,id DESC "
                "LIMIT ? OFFSET ?");
            const std::string actor =
                query.actorId.rfind("admin:", 0) == 0 ? query.actorId.substr(6) : query.actorId;
            select.bind(1, actor);
            select.bind(2, actor);
            select.bind(3, query.action);
            select.bind(4, query.action);
            select.bind(5, query.targetType);
            select.bind(6, query.targetType);
            select.bind(7, query.targetId);
            select.bind(8, query.targetId);
            select.bind(9, query.fromAt);
            select.bind(10, query.fromAt);
            select.bind(11, query.toAt);
            select.bind(12, query.toAt);
            select.bind(13, query.pageSize);
            select.bind(14, static_cast<std::int64_t>(query.page - 1) * query.pageSize);
            std::vector<AuditEvent> events;
            while (select.row())
            {
                events.push_back(AuditEvent{select.integer(0), select.text(1), select.text(2),
                                            select.text(3), select.text(4), select.integer(5)});
            }
            return events;
        });
}

// ---- 建站事务与价格调整 ----
// createStationWithChargers 由服务层包在 withTransaction 内调用：
// 任一设备创建失败抛异常，站点与设备整体回滚，不产生半座电站。
bool PostgresRepository::createStationWithChargers(Station& station, const InitialChargerSpec& spec)
{
    std::vector<Charger> chargers;
    chargers.reserve(static_cast<std::size_t>(spec.count));
    for (int index = 1; index <= spec.count; ++index)
    {
        std::ostringstream suffix;
        suffix.width(2);
        suffix.fill('0');
        suffix << index;
        Charger charger;
        charger.code = station.code + (spec.chargerType == ChargerType::DcFast ? "-DC-" : "-AC-") +
                       suffix.str();
        charger.type = spec.chargerType;
        charger.powerWatt = spec.powerWatt;
        charger.connectorStandard = spec.connectorStandard;
        chargers.push_back(std::move(charger));
    }
    for (const auto& charger : chargers)
    {
        if (chargerCodeExists(charger.code))
            return false;
    }
    if (!addStation(station))
        return false;
    for (auto& charger : chargers)
    {
        charger.stationId = station.id;
        if (!addCharger(charger))
            throw std::runtime_error("atomic charger creation failed");
    }
    return true;
}

std::vector<Station> PostgresRepository::stations(const std::optional<int> status,
                                                  const std::optional<std::string> adcode,
                                                  const std::string& keyword)
{
    std::vector<Station> result;
    for (auto& value : stations())
    {
        if (status && value.enabled != (*status == 1))
            continue;
        if (adcode && value.adcode != *adcode)
            continue;
        if (!keyword.empty() && value.name.find(keyword) == std::string::npos &&
            value.address.find(keyword) == std::string::npos &&
            value.code.find(keyword) == std::string::npos)
            continue;
        result.push_back(std::move(value));
    }
    return result;
}

bool PostgresRepository::addChargers(std::vector<Charger>& chargers)
{
    for (std::size_t outer = 0; outer < chargers.size(); ++outer)
    {
        if (chargerCodeExists(chargers[outer].code))
            return false;
        for (std::size_t inner = 0; inner < outer; ++inner)
        {
            if (chargers[outer].code == chargers[inner].code)
                return false;
        }
    }
    for (auto& charger : chargers)
    {
        if (!addCharger(charger))
            throw std::runtime_error("atomic charger creation failed");
    }
    return true;
}

void PostgresRepository::addTariffVersion(const RegionTariff& tariff)
{
    addTariff(tariff);
}

// 费率区间重叠检查（应用层校验）：数据库 UNIQUE(adcode, effective_from)
// 只保证起始点唯一，区间相交由这里在写入前拒绝。
bool PostgresRepository::tariffOverlaps(const std::string& adcode, const std::int64_t from,
                                        const std::int64_t to)
{
    return useDatabase(this, config_,
                       [&](QSqlDatabase* database)
                       {
                           if (inWriteTransaction(this))
                               advisoryTransactionLock(database, "tariff:" + adcode);
                           Statement query(database,
                                           "SELECT 1 FROM region_tariff WHERE adcode=? AND "
                                           "?<=effective_to AND ?>=effective_from LIMIT 1");
                           query.bind(1, adcode);
                           query.bind(2, from);
                           query.bind(3, to);
                           return query.row();
                       });
}

// 新增调价版本（只追加）：ML_APPROVED 或 MANUAL，adjustment_bp 限 ±2000；
// 返回自增 id。
std::int64_t PostgresRepository::addPriceAdjustment(const PriceAdjustment& adjustment)
{
    return useDatabase(
        this, config_,
        [&](QSqlDatabase* database)
        {
            Statement insert(database, "INSERT INTO price_adjustment(station_id,charger_type,"
                                       "source,adjustment_bp,effective_from,effective_to,reason) "
                                       "VALUES(?,?,?,?,?,?,?) RETURNING id");
            insert.bind(1, adjustment.stationId);
            insert.bind(2, adjustment.chargerType);
            insert.bind(3, adjustment.source);
            insert.bind(4, adjustment.adjustmentBp);
            insert.bind(5, adjustment.effectiveFrom);
            insert.bind(6, adjustment.effectiveTo);
            insert.bind(7, adjustment.reason);
            if (!insert.row())
                throw std::runtime_error("price adjustment insert did not return an id");
            return insert.integer(0);
        });
}

// 生效调价：命中时间窗的最新一条（id 最大者胜）；无则返回 nullopt（用基础价）。
std::optional<PriceAdjustment>
PostgresRepository::effectivePriceAdjustment(const std::int64_t stationId, const int chargerType,
                                             const std::int64_t at)
{
    return useDatabase(
        this, config_,
        [&](QSqlDatabase* database) -> std::optional<PriceAdjustment>
        {
            Statement query(database, "SELECT id,station_id,charger_type,source,adjustment_bp,"
                                      "effective_from,effective_to,reason FROM price_adjustment "
                                      "WHERE station_id=? AND charger_type=? AND effective_from<=? "
                                      "AND effective_to>=? ORDER BY id DESC LIMIT 1");
            query.bind(1, stationId);
            query.bind(2, chargerType);
            query.bind(3, at);
            query.bind(4, at);
            if (!query.row())
                return std::nullopt;
            return PriceAdjustment{query.integer(0),
                                   query.integer(1),
                                   static_cast<int>(query.integer(2)),
                                   query.text(3),
                                   static_cast<int>(query.integer(4)),
                                   query.integer(5),
                                   query.integer(6),
                                   query.text(7)};
        });
}

// ---- 设备命令域：重启/受控释放等运维指令的状态跟踪 ----
void PostgresRepository::addDeviceCommand(const DeviceCommand& command)
{
    useDatabase(this, config_,
                [&](QSqlDatabase* database)
                {
                    Statement insert(
                        database, "INSERT INTO device_command(command_no,charger_id,charger_code,"
                                  "status,reason,actor_id,created_at,execute_at,completed_at,"
                                  "error_summary) VALUES(?,?,?,?,?,?,?,?,?,?)");
                    insert.bind(1, command.commandNo);
                    insert.bind(2, command.chargerId);
                    insert.bind(3, command.chargerCode);
                    insert.bind(4, command.status);
                    insert.bind(5, command.reason);
                    insert.bind(6, command.actorId);
                    insert.bind(7, command.createdAt);
                    insert.bind(8, command.executeAt);
                    bindOptional(insert, 9, command.completedAt);
                    insert.bind(10, command.errorSummary);
                    insert.execute();
                });
}

std::optional<DeviceCommand> PostgresRepository::deviceCommand(const std::string& commandNo)
{
    return useDatabase(
        this, config_,
        [&](QSqlDatabase* database) -> std::optional<DeviceCommand>
        {
            Statement query(database, "SELECT command_no,charger_id,charger_code,status,reason,"
                                      "actor_id,created_at,execute_at,completed_at,error_summary "
                                      "FROM device_command WHERE command_no=?");
            query.bind(1, commandNo);
            if (!query.row())
                return std::nullopt;
            DeviceCommand command;
            command.commandNo = query.text(0);
            command.chargerId = query.integer(1);
            command.chargerCode = query.text(2);
            command.status = query.text(3);
            command.reason = query.text(4);
            command.actorId = query.text(5);
            command.createdAt = query.integer(6);
            command.executeAt = query.integer(7);
            command.completedAt = optionalInteger(query, 8);
            command.errorSummary = query.text(9);
            return command;
        });
}

void PostgresRepository::saveDeviceCommand(const DeviceCommand& command)
{
    useDatabase(this, config_,
                [&](QSqlDatabase* database)
                {
                    Statement update(database, "UPDATE device_command SET status=?,completed_at=?,"
                                               "error_summary=? WHERE command_no=?");
                    update.bind(1, command.status);
                    bindOptional(update, 2, command.completedAt);
                    update.bind(3, command.errorSummary);
                    update.bind(4, command.commandNo);
                    update.execute();
                });
}

// 到期命令扫描：取 PENDING 且 execute_at<=now 的前 100 条，交由运维任务执行。
std::vector<DeviceCommand> PostgresRepository::dueDeviceCommands(const std::int64_t now)
{
    return useDatabase(
        this, config_,
        [&](QSqlDatabase* database)
        {
            Statement query(database, "SELECT command_no,charger_id,charger_code,status,reason,"
                                      "actor_id,created_at,execute_at,completed_at,error_summary "
                                      "FROM device_command WHERE status='PENDING' "
                                      "AND execute_at<=? ORDER BY execute_at,command_no LIMIT 100");
            query.bind(1, now);
            std::vector<DeviceCommand> commands;
            while (query.row())
            {
                DeviceCommand command;
                command.commandNo = query.text(0);
                command.chargerId = query.integer(1);
                command.chargerCode = query.text(2);
                command.status = query.text(3);
                command.reason = query.text(4);
                command.actorId = query.text(5);
                command.createdAt = query.integer(6);
                command.executeAt = query.integer(7);
                command.completedAt = optionalInteger(query, 8);
                command.errorSummary = query.text(9);
                commands.push_back(std::move(command));
            }
            return commands;
        });
}

std::optional<ChargingFlow> PostgresRepository::activeFlowOnCharger(const std::int64_t chargerId)
{
    return useDatabase(
        this, config_,
        [&](QSqlDatabase* database) -> std::optional<ChargingFlow>
        {
            Statement query(database,
                            (std::string("SELECT ") + flowColumns +
                             " FROM charging_flow WHERE charger_id=? AND "
                             "status IN (10,20,30,40,50,80) ORDER BY created_at DESC LIMIT 1")
                                .c_str());
            query.bind(1, chargerId);
            return query.row() ? std::optional<ChargingFlow>(readFlow(query)) : std::nullopt;
        });
}

bool PostgresRepository::stationHasActiveFlow(const std::int64_t stationId)
{
    return useDatabase(this, config_,
                       [&](QSqlDatabase* database)
                       {
                           Statement query(database,
                                           "SELECT 1 FROM charging_flow WHERE station_id=? AND "
                                           "status IN (10,20,30,40,50,80) LIMIT 1");
                           query.bind(1, stationId);
                           return query.row();
                       });
}

// 管理端流程分页查询：状态/站点/设备/用户可组合过滤，游标按创建时间倒序。
AdminFlowPage PostgresRepository::flows(const AdminFlowQuery& query)
{
    return useDatabase(
        this, config_,
        [&](QSqlDatabase* database)
        {
            const std::string predicate =
                " FROM charging_flow WHERE (CAST(? AS INTEGER) IS NULL OR status=?) AND "
                "(CAST(? AS BIGINT) IS NULL OR station_id=?) AND "
                "(CAST(? AS BIGINT) IS NULL OR charger_id=?) AND "
                "(CAST(? AS BIGINT) IS NULL OR user_id=?)";
            Statement count(database, (std::string("SELECT COUNT(*)") + predicate).c_str());
            bindFlowFilters(count, query);
            AdminFlowPage page;
            page.page = query.page;
            page.pageSize = query.pageSize;
            if (count.row())
                page.total = static_cast<int>(count.integer(0));
            Statement select(database, (std::string("SELECT ") + flowColumns + predicate +
                                        " ORDER BY created_at DESC,flow_no DESC LIMIT ? OFFSET ?")
                                           .c_str());
            bindFlowFilters(select, query);
            select.bind(9, query.pageSize);
            select.bind(10, static_cast<std::int64_t>(query.page - 1) * query.pageSize);
            while (select.row())
                page.items.push_back(readFlow(select));
            return page;
        });
}

std::vector<ChargingOrder>
PostgresRepository::settledOrders(const std::int64_t fromAt, const std::int64_t toAt,
                                  const std::optional<std::int64_t> stationId)
{
    return useDatabase(this, config_,
                       [&](QSqlDatabase* database)
                       {
                           const std::string sql =
                               std::string("SELECT ") + orderColumns +
                               " FROM charging_order WHERE status=? AND settled_at IS NOT NULL "
                               "AND settled_at>=? AND settled_at<=? AND (CAST(? AS BIGINT) IS NULL "
                               "OR station_id=?) "
                               "ORDER BY settled_at,order_no";
                           Statement query(database, sql.c_str());
                           query.bind(1, static_cast<int>(FlowStatus::Completed));
                           query.bind(2, fromAt);
                           query.bind(3, toAt);
                           if (stationId)
                           {
                               query.bind(4, *stationId);
                               query.bind(5, *stationId);
                           }
                           else
                           {
                               query.bindNull(4);
                               query.bindNull(5);
                           }
                           std::vector<ChargingOrder> values;
                           while (query.row())
                               values.push_back(readOrder(query));
                           return values;
                       });
}

} // namespace ncs::infrastructure::postgres
