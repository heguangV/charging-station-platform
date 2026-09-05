#include "infrastructure/sqlite/sqlite_repository.h"

#include "core/application/security_crypto.h"

#include <sqlite3.h>

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace ncs::infrastructure::sqlite
{
namespace
{

using namespace ncs::core::application;

constexpr int kLatestSchemaVersion = 7;
constexpr const char* kLatestSchemaChecksum = "ncs-v7-order-analytics";

class Connection final
{
  public:
    explicit Connection(const std::string& path)
    {
        const int result = sqlite3_open_v2(
            path.c_str(), &database_,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
        if (result != SQLITE_OK)
        {
            const std::string message = database_ ? sqlite3_errmsg(database_) : "open failed";
            if (database_)
                sqlite3_close(database_);
            database_ = nullptr;
            throw std::runtime_error("database unavailable: " + message);
        }
        sqlite3_busy_timeout(database_, 5000);
        execute("PRAGMA foreign_keys=ON");
        execute("PRAGMA trusted_schema=OFF");
    }

    ~Connection()
    {
        if (database_)
            sqlite3_close(database_);
    }

    sqlite3* get() const
    {
        return database_;
    }

    void execute(const char* sql) const
    {
        char* error = nullptr;
        if (sqlite3_exec(database_, sql, nullptr, nullptr, &error) != SQLITE_OK)
        {
            const std::string message = error ? error : "SQL execution failed";
            sqlite3_free(error);
            throw std::runtime_error(message);
        }
    }

  private:
    sqlite3* database_ = nullptr;
};

class Statement final
{
  public:
    Statement(sqlite3* database, const char* sql) : database_(database)
    {
        if (sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr) != SQLITE_OK)
        {
            throw std::runtime_error(sqlite3_errmsg(database));
        }
    }

    ~Statement()
    {
        sqlite3_finalize(statement_);
    }

    void bind(const int index, const std::int64_t value)
    {
        verify(sqlite3_bind_int64(statement_, index, value));
    }

    void bind(const int index, const int value)
    {
        verify(sqlite3_bind_int(statement_, index, value));
    }

    void bind(const int index, const double value)
    {
        verify(sqlite3_bind_double(statement_, index, value));
    }

    void bind(const int index, const std::string_view value)
    {
        verify(sqlite3_bind_text(statement_, index, value.data(), static_cast<int>(value.size()),
                                 SQLITE_TRANSIENT));
    }

    void bindNull(const int index)
    {
        verify(sqlite3_bind_null(statement_, index));
    }

    void bindBlob(const int index, const std::vector<unsigned char>& value)
    {
        verify(sqlite3_bind_blob(statement_, index, value.data(), static_cast<int>(value.size()),
                                 SQLITE_TRANSIENT));
    }

    bool row()
    {
        const int result = sqlite3_step(statement_);
        if (result == SQLITE_ROW)
            return true;
        if (result == SQLITE_DONE)
            return false;
        throw std::runtime_error(sqlite3_errmsg(database_));
    }

    void execute()
    {
        if (sqlite3_step(statement_) != SQLITE_DONE)
        {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
    }

    void reset()
    {
        verify(sqlite3_reset(statement_));
        verify(sqlite3_clear_bindings(statement_));
    }

    std::int64_t integer(const int column) const
    {
        return sqlite3_column_int64(statement_, column);
    }

    double real(const int column) const
    {
        return sqlite3_column_double(statement_, column);
    }

    std::string text(const int column) const
    {
        const auto* value = sqlite3_column_text(statement_, column);
        const int size = sqlite3_column_bytes(statement_, column);
        return value ? std::string(reinterpret_cast<const char*>(value), size) : std::string();
    }

    bool isNull(const int column) const
    {
        return sqlite3_column_type(statement_, column) == SQLITE_NULL;
    }

    std::vector<unsigned char> blob(const int column) const
    {
        const auto* value =
            static_cast<const unsigned char*>(sqlite3_column_blob(statement_, column));
        const int size = sqlite3_column_bytes(statement_, column);
        return value && size > 0 ? std::vector<unsigned char>(value, value + size)
                                 : std::vector<unsigned char>();
    }

  private:
    void verify(const int result)
    {
        if (result != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(database_));
    }

    sqlite3* database_ = nullptr;
    sqlite3_stmt* statement_ = nullptr;
};

struct TransactionContext
{
    const SqliteRepository* owner = nullptr;
    sqlite3* database = nullptr;
};

thread_local TransactionContext transactionContext;

template <typename Work>
auto useDatabase(const SqliteRepository* owner, const std::string& path, Work&& work)
{
    if (transactionContext.owner == owner)
    {
        return work(transactionContext.database);
    }
    Connection connection(path);
    return work(connection.get());
}

void execute(sqlite3* database, const char* sql)
{
    char* error = nullptr;
    if (sqlite3_exec(database, sql, nullptr, nullptr, &error) != SQLITE_OK)
    {
        const std::string message = error ? error : "SQL execution failed";
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

void bindOptional(Statement& statement, const int index, const std::optional<std::int64_t> value)
{
    if (value)
        statement.bind(index, *value);
    else
        statement.bindNull(index);
}

void bindOptional(Statement& statement, const int index, const std::optional<std::string>& value)
{
    if (value)
        statement.bind(index, *value);
    else
        statement.bindNull(index);
}

std::optional<std::int64_t> optionalInteger(const Statement& statement, const int column)
{
    return statement.isNull(column) ? std::nullopt
                                    : std::optional<std::int64_t>(statement.integer(column));
}

std::optional<std::string> optionalText(const Statement& statement, const int column)
{
    return statement.isNull(column) ? std::nullopt
                                    : std::optional<std::string>(statement.text(column));
}

UserAccount readUser(Statement& statement)
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

Station readStation(Statement& statement)
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

Charger readCharger(Statement& statement)
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

ChargingFlow readFlow(Statement& statement)
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

void bindFlow(Statement& statement, const ChargingFlow& flow)
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

void bindFlowFilters(Statement& statement, const AdminFlowQuery& query)
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

void bindManagedUserFilters(Statement& statement, const AdminUserQuery& query)
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
    "debt_after_cent,settled_at";

ChargingOrder readOrder(Statement& statement)
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
    return value;
}

void bindOrder(Statement& statement, const ChargingOrder& order)
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
}

std::vector<Role> readAdminRoles(sqlite3* database, const std::int64_t adminId)
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

AdminAccount readAdmin(sqlite3* database, Statement& statement)
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

std::string encodeHorizons(const std::vector<int>& horizons)
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

std::vector<int> decodeHorizons(const std::string_view encoded)
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

MlTask readMlTask(Statement& statement)
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

std::string fileSha256(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("backup file unavailable");
    EVP_MD_CTX* raw = EVP_MD_CTX_new();
    if (!raw)
        throw std::runtime_error("backup digest unavailable");
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(raw, &EVP_MD_CTX_free);
    if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("backup digest unavailable");
    std::array<char, 64 * 1024> buffer{};
    while (input)
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0 &&
            EVP_DigestUpdate(context.get(), buffer.data(), static_cast<std::size_t>(count)) != 1)
        {
            throw std::runtime_error("backup digest unavailable");
        }
    }
    if (!input.eof())
        throw std::runtime_error("backup file read failed");
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digestSize = 0;
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &digestSize) != 1)
        throw std::runtime_error("backup digest unavailable");
    static constexpr char hex[] = "0123456789abcdef";
    std::string encoded(digestSize * 2, '\0');
    for (unsigned int index = 0; index < digestSize; ++index)
    {
        encoded[index * 2] = hex[digest[index] >> 4];
        encoded[index * 2 + 1] = hex[digest[index] & 0x0f];
    }
    return encoded;
}

bool restrictOwnerPermissions(const std::filesystem::path& path, const bool directory)
{
    std::error_code error;
    const auto permissions =
        directory ? std::filesystem::perms::owner_all
                  : std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
    std::filesystem::permissions(path, permissions, std::filesystem::perm_options::replace, error);
#ifdef _WIN32
    (void)error;
    return true;
#else
    return !error;
#endif
}

} // namespace

SqliteRepository::SqliteRepository(std::string databasePath)
    : databasePath_(std::move(databasePath))
{
    initialize();
    refreshReadiness();
}

SqliteRepository::~SqliteRepository() = default;

void SqliteRepository::initialize()
{
    const std::filesystem::path path(databasePath_);
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path());
    Connection connection(databasePath_);
    connection.execute("PRAGMA journal_mode=WAL");
    connection.execute(R"SQL(
BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS schema_version(
  version INTEGER PRIMARY KEY, name TEXT NOT NULL, checksum TEXT NOT NULL, applied_at INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS business_sequence(
  prefix TEXT NOT NULL, utc_day INTEGER NOT NULL, value INTEGER NOT NULL CHECK(value>0),
  PRIMARY KEY(prefix,utc_day));
CREATE TABLE IF NOT EXISTS idempotency_record(
  scope TEXT NOT NULL, idempotency_key TEXT NOT NULL, request_digest TEXT NOT NULL,
  result_status INTEGER, result_content_type TEXT, result_body TEXT,
  expires_at INTEGER NOT NULL, lease_expires_at INTEGER NOT NULL,
  lease_token TEXT NOT NULL, permanent INTEGER NOT NULL CHECK(permanent IN (0,1)),
  PRIMARY KEY(scope,idempotency_key));
CREATE TABLE IF NOT EXISTS user_account(
  id INTEGER PRIMARY KEY AUTOINCREMENT, username TEXT NOT NULL UNIQUE, phone TEXT NOT NULL UNIQUE,
  nickname TEXT NOT NULL, status INTEGER NOT NULL CHECK(status IN (0,1)), registered_at INTEGER NOT NULL,
  balance_cent INTEGER NOT NULL DEFAULT 0 CHECK(balance_cent>=0),
  debt_cent INTEGER NOT NULL DEFAULT 0 CHECK(debt_cent>=0), has_active_flow INTEGER NOT NULL DEFAULT 0 CHECK(has_active_flow IN (0,1)),
  version INTEGER NOT NULL DEFAULT 1 CHECK(version>0), deleted INTEGER NOT NULL DEFAULT 0 CHECK(deleted IN (0,1)));
CREATE TABLE IF NOT EXISTS user_credential(
  user_id INTEGER PRIMARY KEY REFERENCES user_account(id) ON DELETE CASCADE, password_hash TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS user_avatar(
  user_id INTEGER PRIMARY KEY REFERENCES user_account(id) ON DELETE CASCADE,
  data BLOB NOT NULL, content_type TEXT NOT NULL, etag TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS wallet_account(
  user_id INTEGER PRIMARY KEY REFERENCES user_account(id), balance_cent INTEGER NOT NULL CHECK(balance_cent>=0),
  debt_cent INTEGER NOT NULL CHECK(debt_cent>=0), version INTEGER NOT NULL CHECK(version>0), updated_at INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS wallet_transaction(
  id INTEGER PRIMARY KEY AUTOINCREMENT, user_id INTEGER NOT NULL REFERENCES user_account(id), transaction_no TEXT NOT NULL UNIQUE,
  type INTEGER NOT NULL CHECK(type IN (0,1,2)), amount_cent INTEGER NOT NULL, balance_after_cent INTEGER NOT NULL,
  debt_after_cent INTEGER NOT NULL, related_no TEXT NOT NULL, created_at INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS recharge_order(
  recharge_no TEXT PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES user_account(id), requested_cent INTEGER NOT NULL,
  debt_paid_cent INTEGER NOT NULL, balance_added_cent INTEGER NOT NULL, balance_after_cent INTEGER NOT NULL,
  debt_after_cent INTEGER NOT NULL, completed_at INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS station(
  id INTEGER PRIMARY KEY, code TEXT NOT NULL UNIQUE, name TEXT NOT NULL, address TEXT NOT NULL, adcode TEXT NOT NULL,
  latitude_e6 INTEGER NOT NULL, longitude_e6 INTEGER NOT NULL, business_hours TEXT NOT NULL, enabled INTEGER NOT NULL CHECK(enabled IN (0,1)));
CREATE TABLE IF NOT EXISTS charger(
  id INTEGER PRIMARY KEY, station_id INTEGER NOT NULL REFERENCES station(id), code TEXT NOT NULL UNIQUE,
  charger_type INTEGER NOT NULL CHECK(charger_type IN (0,1)), power_watt INTEGER NOT NULL CHECK(power_watt>0),
  connector_standard TEXT NOT NULL, status INTEGER NOT NULL CHECK(status IN (0,1,2,3)),
  total_count INTEGER NOT NULL DEFAULT 0, total_minutes INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS region_tariff(
  id INTEGER PRIMARY KEY AUTOINCREMENT, adcode TEXT NOT NULL, electricity_cent_per_kwh INTEGER NOT NULL,
  service_cent_per_kwh INTEGER NOT NULL, effective_from INTEGER NOT NULL, effective_to INTEGER NOT NULL,
  UNIQUE(adcode,effective_from));
CREATE TABLE IF NOT EXISTS charging_flow(
  flow_no TEXT PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES user_account(id), station_id INTEGER NOT NULL REFERENCES station(id),
  charger_type INTEGER NOT NULL CHECK(charger_type IN (0,1)), charger_id INTEGER REFERENCES charger(id), charger_code TEXT,
  status INTEGER NOT NULL CHECK(status IN (10,20,30,40,50,60,70,80,90)), quote_no TEXT UNIQUE,
  quote_charger_id INTEGER, quote_charger_code TEXT, electricity_price INTEGER, base_service_price INTEGER,
  queue_adjustment_bp INTEGER, ml_adjustment_bp INTEGER, final_service_price INTEGER, total_price INTEGER,
  quote_expires_at INTEGER, reserved_until INTEGER, started_at INTEGER, version INTEGER NOT NULL CHECK(version>0), created_at INTEGER NOT NULL);
CREATE UNIQUE INDEX IF NOT EXISTS uq_active_flow_user ON charging_flow(user_id)
  WHERE status IN (10,20,30,40,50,80);
CREATE UNIQUE INDEX IF NOT EXISTS uq_active_flow_charger ON charging_flow(charger_id)
  WHERE charger_id IS NOT NULL AND status IN (20,30,40,50,80);
CREATE TABLE IF NOT EXISTS flow_event(
  id INTEGER PRIMARY KEY AUTOINCREMENT, flow_no TEXT NOT NULL REFERENCES charging_flow(flow_no),
  from_status INTEGER NOT NULL, to_status INTEGER NOT NULL, reason_code TEXT NOT NULL, at INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS outbox_event(
  id INTEGER PRIMARY KEY AUTOINCREMENT, event_type TEXT NOT NULL, aggregate_type TEXT NOT NULL,
  aggregate_id TEXT NOT NULL, from_status INTEGER NOT NULL, to_status INTEGER NOT NULL,
  reason_code TEXT NOT NULL, created_at INTEGER NOT NULL,
  delivery_status INTEGER NOT NULL DEFAULT 0 CHECK(delivery_status IN (0,1,2)),
  delivery_attempts INTEGER NOT NULL DEFAULT 0 CHECK(delivery_attempts>=0),
  available_at INTEGER NOT NULL, published_at INTEGER);
CREATE INDEX IF NOT EXISTS ix_outbox_delivery
  ON outbox_event(delivery_status,available_at,id);
CREATE TABLE IF NOT EXISTS flow_queue(
  station_id INTEGER NOT NULL REFERENCES station(id), charger_type INTEGER NOT NULL, flow_no TEXT NOT NULL UNIQUE REFERENCES charging_flow(flow_no),
  sequence INTEGER PRIMARY KEY AUTOINCREMENT);
CREATE TABLE IF NOT EXISTS charging_order(
  order_no TEXT PRIMARY KEY, flow_no TEXT NOT NULL UNIQUE REFERENCES charging_flow(flow_no), user_id INTEGER NOT NULL REFERENCES user_account(id),
  station_id INTEGER NOT NULL REFERENCES station(id), station_name TEXT NOT NULL, charger_id INTEGER NOT NULL REFERENCES charger(id),
  charger_code TEXT NOT NULL, charger_type INTEGER NOT NULL, electricity_price INTEGER NOT NULL, service_price INTEGER NOT NULL,
  power_watt INTEGER NOT NULL, time_scale INTEGER NOT NULL, target_amount_cent INTEGER,
  status INTEGER NOT NULL CHECK(status IN (30,40,50,60,70,80,90)), created_at INTEGER NOT NULL,
  started_at INTEGER, ended_at INTEGER, energy_mwh INTEGER NOT NULL DEFAULT 0, amount_cent INTEGER NOT NULL DEFAULT 0,
  paid_cent INTEGER NOT NULL DEFAULT 0, debt_added_cent INTEGER NOT NULL DEFAULT 0,
  balance_after_cent INTEGER NOT NULL DEFAULT 0, debt_after_cent INTEGER NOT NULL DEFAULT 0, settled_at INTEGER);
INSERT OR IGNORE INTO schema_version VALUES(1,'initial-user-charging','ncs-v1',strftime('%s','now'));
INSERT OR IGNORE INTO station(id,code,name,address,adcode,latitude_e6,longitude_e6,business_hours,enabled) VALUES
 (1,'ZGC','NCS 中关村充电站','北京市海淀区中关村大街 27 号','110108',39977680,116316417,'00:00-24:00',1),
 (2,'XEQ','NCS 西二旗充电站','北京市海淀区上地信息路 2 号','110108',40052768,116307517,'00:00-24:00',1),
 (3,'CBD','NCS 国贸充电站','北京市朝阳区建国门外大街 1 号','110105',39908372,116457658,'00:00-24:00',1);
INSERT OR IGNORE INTO region_tariff(adcode,electricity_cent_per_kwh,service_cent_per_kwh,effective_from,effective_to) VALUES
 ('110108',85,50,0,4102444800),('110105',92,48,0,4102444800);
INSERT OR IGNORE INTO charger(id,station_id,code,charger_type,power_watt,connector_standard,status,total_count,total_minutes) VALUES
 (1,1,'ZGC-DC-01',1,60000,'GB/T',0,0,0),(2,1,'ZGC-DC-02',1,60000,'GB/T',0,0,0),
 (3,1,'ZGC-DC-03',1,120000,'GB/T',0,0,0),(4,1,'ZGC-AC-01',0,7000,'GB/T',0,0,0),
 (5,1,'ZGC-AC-02',0,7000,'GB/T',0,0,0),(6,2,'XEQ-DC-01',1,120000,'GB/T',0,0,0),
 (7,2,'XEQ-AC-01',0,7000,'GB/T',0,0,0),(8,3,'CBD-DC-01',1,60000,'GB/T',2,0,0),
 (9,3,'CBD-AC-01',0,7000,'GB/T',0,0,0);
COMMIT;
)SQL");

    Statement migration(connection.get(), "SELECT 1 FROM schema_version WHERE version=2");
    if (!migration.row())
    {
        connection.execute(R"SQL(
BEGIN IMMEDIATE;
ALTER TABLE station ADD COLUMN version INTEGER NOT NULL DEFAULT 1 CHECK(version>0);
ALTER TABLE charger ADD COLUMN version INTEGER NOT NULL DEFAULT 1 CHECK(version>0);
CREATE TABLE admin_account(
  id INTEGER PRIMARY KEY AUTOINCREMENT, username TEXT NOT NULL UNIQUE,
  password_hash TEXT NOT NULL, status INTEGER NOT NULL CHECK(status IN (0,1)),
  must_change_password INTEGER NOT NULL DEFAULT 0 CHECK(must_change_password IN (0,1)),
  version INTEGER NOT NULL DEFAULT 1 CHECK(version>0));
CREATE TABLE admin_role(
  admin_id INTEGER NOT NULL REFERENCES admin_account(id) ON DELETE CASCADE,
  role TEXT NOT NULL CHECK(role IN ('OPERATOR','OWNER','VIEWER')),
  PRIMARY KEY(admin_id,role));
CREATE TABLE ops_log(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  actor_admin_id INTEGER NOT NULL REFERENCES admin_account(id),
  action TEXT NOT NULL, target_type TEXT NOT NULL, target_id TEXT NOT NULL,
  reason TEXT NOT NULL, at INTEGER NOT NULL);
CREATE INDEX ix_ops_log_query ON ops_log(at DESC,actor_admin_id,action);
CREATE TABLE price_adjustment(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  station_id INTEGER NOT NULL REFERENCES station(id),
  charger_type INTEGER NOT NULL CHECK(charger_type IN (0,1)),
  source TEXT NOT NULL CHECK(source IN ('ML_APPROVED','MANUAL')),
  adjustment_bp INTEGER NOT NULL CHECK(adjustment_bp BETWEEN -2000 AND 2000),
  effective_from INTEGER NOT NULL, effective_to INTEGER NOT NULL,
  reason TEXT NOT NULL, CHECK(effective_to>effective_from));
CREATE INDEX ix_price_adjustment_effective
  ON price_adjustment(station_id,charger_type,effective_from,effective_to,id DESC);
CREATE TABLE device_command(
  command_no TEXT PRIMARY KEY, charger_id INTEGER NOT NULL REFERENCES charger(id),
  charger_code TEXT NOT NULL, status TEXT NOT NULL
    CHECK(status IN ('PENDING','RUNNING','SUCCEEDED','FAILED')),
  reason TEXT NOT NULL, actor_id TEXT NOT NULL, created_at INTEGER NOT NULL,
  execute_at INTEGER NOT NULL, completed_at INTEGER, error_summary TEXT NOT NULL DEFAULT '');
CREATE INDEX ix_device_command_due ON device_command(status,execute_at);
CREATE TABLE ml_task(
  task_no TEXT PRIMARY KEY, task_type TEXT NOT NULL CHECK(task_type IN ('TRAIN','PREDICT')),
  status TEXT NOT NULL CHECK(status IN ('PENDING','RUNNING','SUCCEEDED','FAILED','TIMED_OUT')),
  model_version TEXT NOT NULL DEFAULT '', horizon_hours TEXT NOT NULL DEFAULT '',
  created_at INTEGER NOT NULL, finished_at INTEGER,
  metrics_summary TEXT NOT NULL DEFAULT '', error_summary TEXT NOT NULL DEFAULT '');
CREATE INDEX ix_ml_task_running ON ml_task(task_type,status,created_at DESC);
CREATE TABLE backup_record(
  backup_no TEXT PRIMARY KEY, status TEXT NOT NULL,
  checksum TEXT NOT NULL DEFAULT '', size_bytes INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL, verification_status TEXT NOT NULL DEFAULT '',
  verified_at INTEGER, storage_path TEXT NOT NULL DEFAULT '');
INSERT INTO schema_version VALUES(2,'admin-control-plane','ncs-v2-admin-control-plane',strftime('%s','now'));
COMMIT;
)SQL");
    }

    Statement statusMigration(connection.get(), "SELECT 1 FROM schema_version WHERE version=3");
    if (!statusMigration.row())
    {
        connection.execute("PRAGMA foreign_keys=OFF");
        try
        {
            connection.execute(R"SQL(
BEGIN IMMEDIATE;
CREATE TABLE charger_v3(
  id INTEGER PRIMARY KEY, station_id INTEGER NOT NULL REFERENCES station(id),
  code TEXT NOT NULL UNIQUE, charger_type INTEGER NOT NULL CHECK(charger_type IN (0,1)),
  power_watt INTEGER NOT NULL CHECK(power_watt>0), connector_standard TEXT NOT NULL,
  status INTEGER NOT NULL CHECK(status IN (0,1,2,3,4)),
  total_count INTEGER NOT NULL DEFAULT 0, total_minutes INTEGER NOT NULL DEFAULT 0,
  version INTEGER NOT NULL DEFAULT 1 CHECK(version>0));
INSERT INTO charger_v3(id,station_id,code,charger_type,power_watt,
  connector_standard,status,total_count,total_minutes,version)
  SELECT id,station_id,code,charger_type,power_watt,connector_standard,status,
    total_count,total_minutes,version FROM charger;
DROP TABLE charger;
ALTER TABLE charger_v3 RENAME TO charger;
INSERT INTO schema_version VALUES(3,'charger-restarting-state','ncs-v3-restarting',strftime('%s','now'));
COMMIT;
)SQL");
            connection.execute("PRAGMA foreign_keys=ON");
            Statement foreignKeys(connection.get(), "PRAGMA foreign_key_check");
            if (foreignKeys.row())
                throw std::runtime_error("foreign-key check failed after migration");
        }
        catch (...)
        {
            try
            {
                connection.execute("ROLLBACK");
            }
            catch (...)
            {
            }
            try
            {
                connection.execute("PRAGMA foreign_keys=ON");
            }
            catch (...)
            {
            }
            throw;
        }
    }

    Statement demoMigration(connection.get(), "SELECT 1 FROM schema_version WHERE version=4");
    if (!demoMigration.row())
    {
        connection.execute(R"SQL(
BEGIN IMMEDIATE;
ALTER TABLE admin_account ADD COLUMN is_demo INTEGER NOT NULL DEFAULT 0 CHECK(is_demo IN (0,1));
INSERT INTO schema_version VALUES(4,'development-admin-marker','ncs-v4-demo-admin',strftime('%s','now'));
COMMIT;
)SQL");
    }

    Statement indexMigration(connection.get(), "SELECT 1 FROM schema_version WHERE version=5");
    if (!indexMigration.row())
    {
        connection.execute(R"SQL(
BEGIN IMMEDIATE;
CREATE INDEX IF NOT EXISTS ix_flow_created ON charging_flow(created_at,flow_no);
CREATE INDEX IF NOT EXISTS ix_flow_station_active
  ON charging_flow(station_id) WHERE status IN (10,20,30,40,50,80);
CREATE INDEX IF NOT EXISTS ix_flow_charger_active
  ON charging_flow(charger_id) WHERE charger_id IS NOT NULL AND status IN (10,20,30,40,50,80);
INSERT INTO schema_version VALUES(5,'admin-query-indexes','ncs-v5-admin-indexes',strftime('%s','now'));
COMMIT;
)SQL");
    }

    Statement analyticsMigration(connection.get(), "SELECT 1 FROM schema_version WHERE version=6");
    if (!analyticsMigration.row())
    {
        connection.execute(R"SQL(
BEGIN IMMEDIATE;
UPDATE ml_task SET status='FAILED',finished_at=strftime('%s','now'),
  error_summary='迁移时清理重复运行任务'
WHERE status IN ('PENDING','RUNNING') AND EXISTS(
  SELECT 1 FROM ml_task newer WHERE newer.task_type=ml_task.task_type
    AND newer.status IN ('PENDING','RUNNING')
    AND (newer.created_at>ml_task.created_at OR
      (newer.created_at=ml_task.created_at AND newer.task_no>ml_task.task_no)));
CREATE UNIQUE INDEX IF NOT EXISTS ux_ml_task_one_running_type
  ON ml_task(task_type) WHERE status IN ('PENDING','RUNNING');
CREATE TABLE station_hourly_metric(
  station_id INTEGER NOT NULL REFERENCES station(id),
  bucket_at INTEGER NOT NULL,
  energy_mwh INTEGER NOT NULL CHECK(energy_mwh>=0),
  order_count INTEGER NOT NULL CHECK(order_count>=0),
  fast_order_count INTEGER NOT NULL CHECK(fast_order_count>=0),
  slow_order_count INTEGER NOT NULL CHECK(slow_order_count>=0),
  busy_device_seconds INTEGER NOT NULL CHECK(busy_device_seconds>=0),
  refreshed_at INTEGER NOT NULL,
  PRIMARY KEY(station_id,bucket_at));
CREATE INDEX ix_station_hourly_bucket
  ON station_hourly_metric(bucket_at,station_id);
CREATE TABLE model_version(
  version_no TEXT PRIMARY KEY,
  task_no TEXT NOT NULL UNIQUE REFERENCES ml_task(task_no),
  algorithm TEXT NOT NULL,
  feature_schema_version INTEGER NOT NULL,
  random_seed INTEGER NOT NULL,
  train_from_at INTEGER NOT NULL,
  train_to_at INTEGER NOT NULL,
  mae REAL NOT NULL CHECK(mae>=0), rmse REAL NOT NULL CHECK(rmse>=0),
  mape REAL NOT NULL CHECK(mape>=0), wape REAL NOT NULL CHECK(wape>=0),
  baseline_mae REAL NOT NULL CHECK(baseline_mae>=0),
  baseline_rmse REAL NOT NULL CHECK(baseline_rmse>=0),
  excluded_sample_count INTEGER NOT NULL CHECK(excluded_sample_count>=0),
  qualified INTEGER NOT NULL CHECK(qualified IN (0,1)),
  artifact_checksum TEXT NOT NULL,
  artifact_path TEXT NOT NULL,
  created_at INTEGER NOT NULL);
CREATE INDEX ix_model_version_retention ON model_version(created_at);
CREATE TABLE load_prediction(
  station_id INTEGER NOT NULL REFERENCES station(id),
  model_version_no TEXT NOT NULL,
  generated_at INTEGER NOT NULL,
  target_at INTEGER NOT NULL,
  horizon_hour INTEGER NOT NULL CHECK(horizon_hour IN (1,6,24)),
  predicted_energy_mwh INTEGER NOT NULL CHECK(predicted_energy_mwh>=0),
  predicted_free_count INTEGER NOT NULL CHECK(predicted_free_count>=0),
  is_peak INTEGER NOT NULL CHECK(is_peak IN (0,1)),
  stale INTEGER NOT NULL DEFAULT 0 CHECK(stale IN (0,1)),
  PRIMARY KEY(station_id,model_version_no,target_at));
CREATE INDEX ix_load_prediction_query
  ON load_prediction(target_at,horizon_hour,station_id);
CREATE TABLE dashboard_state(
  singleton INTEGER PRIMARY KEY CHECK(singleton=1),
  data_version INTEGER NOT NULL CHECK(data_version>=0));
INSERT INTO dashboard_state(singleton,data_version) VALUES(1,0);
INSERT INTO schema_version VALUES(6,'dashboard-ml','ncs-v6-dashboard-ml',strftime('%s','now'));
COMMIT;
)SQL");
    }

    Statement orderIndexMigration(connection.get(), "SELECT 1 FROM schema_version WHERE version=7");
    if (!orderIndexMigration.row())
    {
        connection.execute(R"SQL(
BEGIN IMMEDIATE;
CREATE INDEX IF NOT EXISTS ix_order_status_settled
  ON charging_order(status,settled_at);
CREATE INDEX IF NOT EXISTS ix_order_status_started
  ON charging_order(status,COALESCE(started_at,created_at));
INSERT INTO schema_version VALUES(7,'order-analytics-indexes','ncs-v7-order-analytics',strftime('%s','now'));
COMMIT;
)SQL");
    }
}

std::optional<UserAccount> SqliteRepository::findById(const std::int64_t id) const
{
    return useDatabase(
        this, databasePath_,
        [&](sqlite3* database) -> std::optional<UserAccount>
        {
            Statement query(database,
                            (std::string(userSelect) + "WHERE u.id=? AND u.deleted=0").c_str());
            query.bind(1, id);
            return query.row() ? std::optional<UserAccount>(readUser(query)) : std::nullopt;
        });
}

std::optional<UserAccount> SqliteRepository::findByPhone(const std::string_view phone) const
{
    return useDatabase(
        this, databasePath_,
        [&](sqlite3* database) -> std::optional<UserAccount>
        {
            Statement query(database,
                            (std::string(userSelect) + "WHERE u.phone=? AND u.deleted=0").c_str());
            query.bind(1, phone);
            return query.row() ? std::optional<UserAccount>(readUser(query)) : std::nullopt;
        });
}

std::optional<UserAccount> SqliteRepository::findByLoginName(const std::string_view loginName) const
{
    return useDatabase(
        this, databasePath_,
        [&](sqlite3* database) -> std::optional<UserAccount>
        {
            Statement query(database, (std::string(userSelect) +
                                       "WHERE (u.username=? OR u.phone=?) AND u.deleted=0")
                                          .c_str());
            query.bind(1, loginName);
            query.bind(2, loginName);
            return query.row() ? std::optional<UserAccount>(readUser(query)) : std::nullopt;
        });
}

AccountWriteResult SqliteRepository::create(UserAccount& account)
{
    try
    {
        withTransaction(
            [&]
            {
                Statement insert(transactionContext.database,
                                 "INSERT INTO "
                                 "user_account(username,phone,nickname,status,registered_"
                                 "at,balance_cent,debt_cent,has_active_flow,version,"
                                 "deleted) VALUES(?,?,?,?,?,?,?,?,?,0)");
                insert.bind(1, account.username);
                insert.bind(2, account.phone);
                insert.bind(3, account.nickname);
                insert.bind(4, account.status);
                insert.bind(5, account.registeredAt);
                insert.bind(6, account.balanceCent);
                insert.bind(7, account.debtCent);
                insert.bind(8, account.hasActiveFlow ? 1 : 0);
                insert.bind(9, account.version);
                insert.execute();
                account.id = sqlite3_last_insert_rowid(transactionContext.database);
                if (account.passwordHash)
                {
                    Statement credential(
                        transactionContext.database,
                        "INSERT INTO user_credential(user_id,password_hash) VALUES(?,?)");
                    credential.bind(1, account.id);
                    credential.bind(2, *account.passwordHash);
                    credential.execute();
                }
                Statement walletInsert(transactionContext.database,
                                       "INSERT INTO "
                                       "wallet_account(user_id,balance_cent,debt_cent,"
                                       "version,updated_at) VALUES(?,?,?,?,?)");
                walletInsert.bind(1, account.id);
                walletInsert.bind(2, account.balanceCent);
                walletInsert.bind(3, account.debtCent);
                walletInsert.bind(4, 1);
                walletInsert.bind(5, account.registeredAt);
                walletInsert.execute();
            });
        return AccountWriteResult::Success;
    }
    catch (const std::exception&)
    {
        if (findByLoginName(account.username))
            return AccountWriteResult::UsernameExists;
        if (findByPhone(account.phone))
            return AccountWriteResult::PhoneExists;
        throw;
    }
}

AccountWriteResult SqliteRepository::updateNickname(const std::int64_t id,
                                                    const std::int64_t expectedVersion,
                                                    std::string nickname, UserAccount& updated)
{
    return useDatabase(this, databasePath_,
                       [&](sqlite3* database)
                       {
                           Statement update(database,
                                            "UPDATE user_account SET nickname=?,version=version+1 "
                                            "WHERE id=? AND version=? AND deleted=0");
                           update.bind(1, nickname);
                           update.bind(2, id);
                           update.bind(3, expectedVersion);
                           update.execute();
                           if (sqlite3_changes(database) == 0)
                               return findById(id) ? AccountWriteResult::VersionConflict
                                                   : AccountWriteResult::NotFound;
                           updated = *findById(id);
                           return AccountWriteResult::Success;
                       });
}

AccountWriteResult SqliteRepository::updateStatus(const std::int64_t id, const int status,
                                                  UserAccount& updated)
{
    if (status != 0 && status != 1)
        return AccountWriteResult::NotFound;
    return useDatabase(this, databasePath_,
                       [&](sqlite3* database)
                       {
                           Statement update(
                               database, "UPDATE user_account SET status=?,version=version+1 WHERE "
                                         "id=? AND deleted=0");
                           update.bind(1, status);
                           update.bind(2, id);
                           update.execute();
                           if (sqlite3_changes(database) == 0)
                               return AccountWriteResult::NotFound;
                           updated = *findById(id);
                           return AccountWriteResult::Success;
                       });
}

AccountWriteResult SqliteRepository::updateCredential(const std::int64_t id, std::string username,
                                                      std::string passwordHash,
                                                      UserAccount& updated)
{
    try
    {
        withTransaction(
            [&]
            {
                Statement update(transactionContext.database,
                                 "UPDATE user_account SET username=?,version=version+1 "
                                 "WHERE id=? AND deleted=0");
                update.bind(1, username);
                update.bind(2, id);
                update.execute();
                if (sqlite3_changes(transactionContext.database) == 0)
                    throw std::out_of_range("user");
                Statement credential(transactionContext.database,
                                     "INSERT INTO user_credential(user_id,password_hash) "
                                     "VALUES(?,?) ON CONFLICT(user_id) DO UPDATE SET "
                                     "password_hash=excluded.password_hash");
                credential.bind(1, id);
                credential.bind(2, passwordHash);
                credential.execute();
            });
    }
    catch (const std::out_of_range&)
    {
        return AccountWriteResult::NotFound;
    }
    catch (const std::exception&)
    {
        const auto existing = findByLoginName(username);
        if (existing && existing->id != id)
            return AccountWriteResult::UsernameExists;
        throw;
    }
    updated = *findById(id);
    return AccountWriteResult::Success;
}

AccountWriteResult SqliteRepository::replacePasswordHash(const std::int64_t id,
                                                         const std::string_view expectedCurrentHash,
                                                         const std::string_view newPasswordHash)
{
    return useDatabase(this, databasePath_,
                       [&](sqlite3* database)
                       {
                           Statement update(database, "UPDATE user_credential SET password_hash=? "
                                                      "WHERE user_id=? AND password_hash=?");
                           update.bind(1, newPasswordHash);
                           update.bind(2, id);
                           update.bind(3, expectedCurrentHash);
                           update.execute();
                           if (sqlite3_changes(database) > 0)
                               return AccountWriteResult::Success;
                           return findById(id) ? AccountWriteResult::VersionConflict
                                               : AccountWriteResult::NotFound;
                       });
}

AccountWriteResult SqliteRepository::updateAvatar(const std::int64_t id, AvatarData avatar,
                                                  UserAccount& updated)
{
    if (!findById(id))
        return AccountWriteResult::NotFound;
    withTransaction(
        [&]
        {
            Statement statement(
                transactionContext.database,
                "INSERT INTO user_avatar(user_id,data,content_type,etag) "
                "VALUES(?,?,?,?) ON CONFLICT(user_id) DO UPDATE SET "
                "data=excluded.data,content_type=excluded.content_type,etag=excluded."
                "etag");
            statement.bind(1, id);
            statement.bindBlob(2, avatar.bytes);
            statement.bind(3, avatar.contentType);
            statement.bind(4, avatar.etag);
            statement.execute();
            Statement version(transactionContext.database,
                              "UPDATE user_account SET version=version+1 WHERE id=? AND deleted=0");
            version.bind(1, id);
            version.execute();
        });
    updated = *findById(id);
    return AccountWriteResult::Success;
}

AccountWriteResult SqliteRepository::anonymize(const std::int64_t id, UserAccount& updated)
{
    AccountWriteResult result = AccountWriteResult::NotFound;
    withTransaction(
        [&]
        {
            Statement current(transactionContext.database,
                              "SELECT deleted,has_active_flow FROM user_account "
                              "WHERE id=?");
            current.bind(1, id);
            if (!current.row() || current.integer(0) != 0)
                return;
            if (current.integer(1) != 0)
            {
                result = AccountWriteResult::ActiveFlowExists;
                return;
            }
            const std::string suffix = std::to_string(id);
            Statement statement(transactionContext.database,
                                "UPDATE user_account SET "
                                "username=?,phone=?,nickname='已注销用户',status=0,"
                                "deleted=1,version=version+1 WHERE id=? AND deleted=0");
            statement.bind(1, "deleted_user_" + suffix);
            statement.bind(2, "deleted_phone_" + suffix);
            statement.bind(3, id);
            statement.execute();
            Statement credential(transactionContext.database,
                                 "DELETE FROM user_credential WHERE user_id=?");
            credential.bind(1, id);
            credential.execute();
            Statement avatar(transactionContext.database,
                             "DELETE FROM user_avatar WHERE user_id=?");
            avatar.bind(1, id);
            avatar.execute();
            result = AccountWriteResult::Success;
        });
    if (result != AccountWriteResult::Success)
        return result;
    updated.id = id;
    updated.deleted = true;
    return result;
}

void SqliteRepository::applyWalletState(const std::int64_t userId, const std::int64_t balanceCent,
                                        const std::int64_t debtCent)
{
    useDatabase(this, databasePath_,
                [&](sqlite3* database)
                {
                    Statement update(
                        database, "UPDATE user_account SET balance_cent=?,debt_cent=? WHERE id=?");
                    update.bind(1, balanceCent);
                    update.bind(2, debtCent);
                    update.bind(3, userId);
                    update.execute();
                });
}

void SqliteRepository::setActiveFlowFlag(const std::int64_t userId, const bool hasActiveFlow)
{
    useDatabase(this, databasePath_,
                [&](sqlite3* database)
                {
                    Statement update(database,
                                     "UPDATE user_account SET has_active_flow=? WHERE id=?");
                    update.bind(1, hasActiveFlow ? 1 : 0);
                    update.bind(2, userId);
                    update.execute();
                });
}

void SqliteRepository::withTransaction(const std::function<void()>& work)
{
    if (transactionContext.owner == this)
    {
        work();
        return;
    }
    Connection connection(databasePath_);
    execute(connection.get(), "BEGIN IMMEDIATE");
    transactionContext = {this, connection.get()};
    try
    {
        work();
        execute(connection.get(), "COMMIT");
        transactionContext = {};
    }
    catch (...)
    {
        transactionContext = {};
        try
        {
            execute(connection.get(), "ROLLBACK");
        }
        catch (...)
        {
        }
        throw;
    }
}

void SqliteRepository::withReadTransaction(const std::function<void()>& work)
{
    if (transactionContext.owner == this)
    {
        work();
        return;
    }
    Connection connection(databasePath_);
    execute(connection.get(), "BEGIN");
    transactionContext = {this, connection.get()};
    try
    {
        work();
        execute(connection.get(), "COMMIT");
        transactionContext = {};
    }
    catch (...)
    {
        transactionContext = {};
        try
        {
            execute(connection.get(), "ROLLBACK");
        }
        catch (...)
        {
        }
        throw;
    }
}

WalletAccount SqliteRepository::wallet(const std::int64_t userId)
{
    return useDatabase(
        this, databasePath_,
        [&](sqlite3* database)
        {
            Statement query(database, "SELECT user_id,balance_cent,debt_cent,version,updated_at "
                                      "FROM wallet_account WHERE user_id=?");
            query.bind(1, userId);
            if (!query.row())
                throw std::runtime_error("wallet not found");
            return WalletAccount{query.integer(0), query.integer(1), query.integer(2),
                                 query.integer(3), query.integer(4)};
        });
}

void SqliteRepository::saveWallet(const WalletAccount& walletValue)
{
    useDatabase(this, databasePath_,
                [&](sqlite3* database)
                {
                    Statement update(
                        database,
                        "UPDATE wallet_account SET "
                        "balance_cent=?,debt_cent=?,version=?,updated_at=? WHERE user_id=?");
                    update.bind(1, walletValue.balanceCent);
                    update.bind(2, walletValue.debtCent);
                    update.bind(3, walletValue.version);
                    update.bind(4, walletValue.updatedAt);
                    update.bind(5, walletValue.userId);
                    update.execute();
                    if (sqlite3_changes(database) == 0)
                        throw std::runtime_error("wallet update failed");
                });
}

void SqliteRepository::addWalletTransaction(const WalletTransaction& value)
{
    useDatabase(this, databasePath_,
                [&](sqlite3* database)
                {
                    Statement insert(database,
                                     "INSERT INTO "
                                     "wallet_transaction(user_id,transaction_no,type,amount_"
                                     "cent,balance_after_cent,debt_after_cent,related_no,"
                                     "created_at) VALUES(?,?,?,?,?,?,?,?)");
                    insert.bind(1, value.userId);
                    insert.bind(2, value.transactionNo);
                    insert.bind(3, static_cast<int>(value.type));
                    insert.bind(4, value.amountCent);
                    insert.bind(5, value.balanceAfterCent);
                    insert.bind(6, value.debtAfterCent);
                    insert.bind(7, value.relatedNo);
                    insert.bind(8, value.createdAt);
                    insert.execute();
                });
}

std::vector<WalletTransaction>
SqliteRepository::walletTransactions(const std::int64_t userId,
                                     const std::optional<WalletTransactionType> type,
                                     const std::int64_t fromAt, const std::int64_t toAt)
{
    return useDatabase(
        this, databasePath_,
        [&](sqlite3* database)
        {
            Statement query(database,
                            "SELECT "
                            "id,user_id,transaction_no,type,amount_cent,balance_after_cent,debt_"
                            "after_cent,related_no,created_at FROM wallet_transaction WHERE "
                            "user_id=? AND (? IS NULL OR type=?) AND created_at>=? AND (?=0 OR "
                            "created_at<=?) ORDER BY created_at DESC,id DESC");
            query.bind(1, userId);
            if (type)
                query.bind(2, static_cast<int>(*type));
            else
                query.bindNull(2);
            if (type)
                query.bind(3, static_cast<int>(*type));
            else
                query.bindNull(3);
            query.bind(4, fromAt);
            query.bind(5, toAt);
            query.bind(6, toAt);
            std::vector<WalletTransaction> values;
            while (query.row())
                values.push_back(WalletTransaction{
                    query.integer(0), query.integer(1), query.text(2),
                    static_cast<WalletTransactionType>(query.integer(3)), query.integer(4),
                    query.integer(5), query.integer(6), query.text(7), query.integer(8)});
            return values;
        });
}

void SqliteRepository::addRechargeOrder(const RechargeOrder& value)
{
    useDatabase(this, databasePath_,
                [&](sqlite3* database)
                {
                    Statement insert(database,
                                     "INSERT INTO recharge_order VALUES(?,?,?,?,?,?,?,?)");
                    insert.bind(1, value.rechargeNo);
                    insert.bind(2, value.userId);
                    insert.bind(3, value.requestedCent);
                    insert.bind(4, value.debtPaidCent);
                    insert.bind(5, value.balanceAddedCent);
                    insert.bind(6, value.balanceAfterCent);
                    insert.bind(7, value.debtAfterCent);
                    insert.bind(8, value.completedAt);
                    insert.execute();
                });
}

std::vector<Station> SqliteRepository::stations()
{
    return useDatabase(this, databasePath_,
                       [&](sqlite3* database)
                       {
                           Statement query(
                               database, "SELECT "
                                         "id,code,name,address,adcode,latitude_e6,longitude_e6,"
                                         "business_hours,enabled,version FROM station ORDER BY id");
                           std::vector<Station> values;
                           while (query.row())
                               values.push_back(readStation(query));
                           return values;
                       });
}

std::optional<Station> SqliteRepository::station(const std::int64_t stationId)
{
    return useDatabase(
        this, databasePath_,
        [&](sqlite3* database) -> std::optional<Station>
        {
            Statement query(database, "SELECT "
                                      "id,code,name,address,adcode,latitude_e6,longitude_e6,"
                                      "business_hours,enabled,version FROM station WHERE id=?");
            query.bind(1, stationId);
            return query.row() ? std::optional<Station>(readStation(query)) : std::nullopt;
        });
}

std::vector<Charger> SqliteRepository::chargers(const std::optional<std::int64_t> stationId,
                                                const std::optional<ChargerType> type,
                                                const std::optional<ChargerStatus> status)
{
    return useDatabase(this, databasePath_,
                       [&](sqlite3* database)
                       {
                           Statement query(
                               database,
                               "SELECT "
                               "id,station_id,code,charger_type,power_watt,connector_"
                               "standard,status,total_count,total_minutes,version FROM charger "
                               "WHERE (? IS NULL OR station_id=?) AND (? IS NULL OR "
                               "charger_type=?) AND (? IS NULL OR status=?) ORDER BY id");
                           if (stationId)
                               query.bind(1, *stationId);
                           else
                               query.bindNull(1);
                           if (stationId)
                               query.bind(2, *stationId);
                           else
                               query.bindNull(2);
                           if (type)
                               query.bind(3, static_cast<int>(*type));
                           else
                               query.bindNull(3);
                           if (type)
                               query.bind(4, static_cast<int>(*type));
                           else
                               query.bindNull(4);
                           if (status)
                               query.bind(5, static_cast<int>(*status));
                           else
                               query.bindNull(5);
                           if (status)
                               query.bind(6, static_cast<int>(*status));
                           else
                               query.bindNull(6);
                           std::vector<Charger> values;
                           while (query.row())
                               values.push_back(readCharger(query));
                           return values;
                       });
}

std::optional<Charger> SqliteRepository::charger(const std::int64_t chargerId)
{
    return useDatabase(
        this, databasePath_,
        [&](sqlite3* database) -> std::optional<Charger>
        {
            Statement query(database,
                            "SELECT "
                            "id,station_id,code,charger_type,power_watt,connector_standard,"
                            "status,total_count,total_minutes,version FROM charger WHERE id=?");
            query.bind(1, chargerId);
            return query.row() ? std::optional<Charger>(readCharger(query)) : std::nullopt;
        });
}

void SqliteRepository::saveCharger(const Charger& value)
{
    useDatabase(this, databasePath_,
                [&](sqlite3* database)
                {
                    Statement update(
                        database,
                        "UPDATE charger SET "
                        "station_id=?,code=?,charger_type=?,power_watt=?,connector_standard=?,"
                        "status=?,total_count=?,total_minutes=?,version=? WHERE id=?");
                    update.bind(1, value.stationId);
                    update.bind(2, value.code);
                    update.bind(3, static_cast<int>(value.type));
                    update.bind(4, value.powerWatt);
                    update.bind(5, value.connectorStandard);
                    update.bind(6, static_cast<int>(value.status));
                    update.bind(7, value.totalCount);
                    update.bind(8, value.totalMinutes);
                    update.bind(9, value.version);
                    update.bind(10, value.id);
                    update.execute();
                });
}

std::optional<RegionTariff> SqliteRepository::effectiveTariff(const std::string& adcode,
                                                              const std::int64_t at)
{
    return useDatabase(this, databasePath_,
                       [&](sqlite3* database) -> std::optional<RegionTariff>
                       {
                           Statement query(database,
                                           "SELECT "
                                           "adcode,electricity_cent_per_kwh,service_cent_per_kwh,"
                                           "effective_from,effective_to FROM region_tariff WHERE "
                                           "adcode=? AND effective_from<=? AND effective_to>=? "
                                           "ORDER BY effective_from DESC LIMIT 1");
                           query.bind(1, adcode);
                           query.bind(2, at);
                           query.bind(3, at);
                           if (!query.row())
                               return std::nullopt;
                           return RegionTariff{query.text(0), static_cast<int>(query.integer(1)),
                                               static_cast<int>(query.integer(2)), query.integer(3),
                                               query.integer(4)};
                       });
}

void SqliteRepository::addFlow(const ChargingFlow& value)
{
    useDatabase(this, databasePath_,
                [&](sqlite3* database)
                {
                    const std::string sql = std::string("INSERT INTO charging_flow(") +
                                            flowColumns +
                                            ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
                    Statement insert(database, sql.c_str());
                    bindFlow(insert, value);
                    insert.execute();
                });
}

void SqliteRepository::saveFlow(const ChargingFlow& value)
{
    useDatabase(this, databasePath_,
                [&](sqlite3* database)
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

std::optional<ChargingFlow> SqliteRepository::flow(const std::string& flowNo)
{
    return useDatabase(this, databasePath_,
                       [&](sqlite3* database) -> std::optional<ChargingFlow>
                       {
                           const std::string sql = std::string("SELECT ") + flowColumns +
                                                   " FROM charging_flow WHERE flow_no=?";
                           Statement query(database, sql.c_str());
                           query.bind(1, flowNo);
                           return query.row() ? std::optional<ChargingFlow>(readFlow(query))
                                              : std::nullopt;
                       });
}

std::optional<ChargingFlow> SqliteRepository::activeFlow(const std::int64_t userId)
{
    return useDatabase(
        this, databasePath_,
        [&](sqlite3* database) -> std::optional<ChargingFlow>
        {
            const std::string sql = std::string("SELECT ") + flowColumns +
                                    " FROM charging_flow WHERE user_id=? AND status IN "
                                    "(10,20,30,40,50,80) ORDER BY created_at DESC LIMIT 1";
            Statement query(database, sql.c_str());
            query.bind(1, userId);
            return query.row() ? std::optional<ChargingFlow>(readFlow(query)) : std::nullopt;
        });
}

std::vector<ChargingFlow> SqliteRepository::flowsWithStatus(const int status)
{
    return useDatabase(this, databasePath_,
                       [&](sqlite3* database)
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

void SqliteRepository::addFlowEvent(const FlowEvent& value)
{
    useDatabase(
        this, databasePath_,
        [&](sqlite3* database)
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
            outbox.bind(1, value.toStatus == static_cast<int>(FlowStatus::Completed)
                               ? std::string_view("order.settled")
                               : std::string_view("flow.updated"));
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

void SqliteRepository::addChargerStatusEvent(const ChargerStatusEvent& value)
{
    useDatabase(this, databasePath_,
                [&](sqlite3* database)
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

std::vector<OutboxEvent> SqliteRepository::pollOutbox(const std::int64_t now, const int limit)
{
    std::vector<OutboxEvent> result;
    useDatabase(this, databasePath_,
                [&](sqlite3* database)
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

void SqliteRepository::markOutboxDelivered(const std::vector<std::int64_t>& ids)
{
    if (ids.empty())
        return;
    const std::int64_t publishedAt = std::chrono::duration_cast<std::chrono::seconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count();
    useDatabase(this, databasePath_,
                [&](sqlite3* database)
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

void SqliteRepository::markOutboxAttempted(const std::vector<std::int64_t>& ids)
{
    if (ids.empty())
        return;
    const std::int64_t nowSeconds = std::chrono::duration_cast<std::chrono::seconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count();
    useDatabase(
        this, databasePath_,
        [&](sqlite3* database)
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
                    "available_at=MAX(available_at,?+MIN(300,5*(1<<MIN(delivery_attempts,6)))),"
                    "delivery_status=CASE WHEN delivery_attempts+1>=10 THEN 2 "
                    "ELSE delivery_status END WHERE id=?");
                update.bind(1, nowSeconds);
                update.bind(2, id);
                update.execute();
            }
        });
}

void SqliteRepository::markOutboxDead(const std::vector<std::int64_t>& ids)
{
    if (ids.empty())
        return;
    useDatabase(this, databasePath_,
                [&](sqlite3* database)
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

void SqliteRepository::enqueue(const std::int64_t stationId, const ChargerType type,
                               const std::string& flowNo)
{
    useDatabase(this, databasePath_,
                [&](sqlite3* database)
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

void SqliteRepository::dequeue(const std::int64_t stationId, const ChargerType type,
                               const std::string& flowNo)
{
    useDatabase(this, databasePath_,
                [&](sqlite3* database)
                {
                    Statement remove(database, "DELETE FROM flow_queue WHERE station_id=? AND "
                                               "charger_type=? AND flow_no=?");
                    remove.bind(1, stationId);
                    remove.bind(2, static_cast<int>(type));
                    remove.bind(3, flowNo);
                    remove.execute();
                });
}

std::deque<std::string> SqliteRepository::queue(const std::int64_t stationId,
                                                const ChargerType type)
{
    return useDatabase(this, databasePath_,
                       [&](sqlite3* database)
                       {
                           Statement query(database,
                                           "SELECT flow_no FROM flow_queue WHERE station_id=? AND "
                                           "charger_type=? ORDER BY sequence");
                           query.bind(1, stationId);
                           query.bind(2, static_cast<int>(type));
                           std::deque<std::string> values;
                           while (query.row())
                               values.push_back(query.text(0));
                           return values;
                       });
}

void SqliteRepository::addOrder(const ChargingOrder& value)
{
    useDatabase(this, databasePath_,
                [&](sqlite3* database)
                {
                    const std::string sql =
                        std::string("INSERT INTO charging_order(") + orderColumns +
                        ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
                    Statement insert(database, sql.c_str());
                    bindOrder(insert, value);
                    insert.execute();
                });
}

void SqliteRepository::saveOrder(const ChargingOrder& value)
{
    useDatabase(this, databasePath_,
                [&](sqlite3* database)
                {
                    Statement update(
                        database,
                        "UPDATE charging_order SET "
                        "flow_no=?,user_id=?,station_id=?,station_name=?,charger_id=?,charger_"
                        "code=?,charger_type=?,electricity_price=?,service_price=?,power_watt=?"
                        ",time_scale=?,target_amount_cent=?,status=?,created_at=?,started_at=?,"
                        "ended_at=?,energy_mwh=?,amount_cent=?,paid_cent=?,debt_added_cent=?,"
                        "balance_after_cent=?,debt_after_cent=?,settled_at=? WHERE order_no=?");
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
                    update.bind(24, value.orderNo);
                    update.execute();
                });
}

std::optional<ChargingOrder> SqliteRepository::order(const std::string& orderNo)
{
    return useDatabase(this, databasePath_,
                       [&](sqlite3* database) -> std::optional<ChargingOrder>
                       {
                           const std::string sql = std::string("SELECT ") + orderColumns +
                                                   " FROM charging_order WHERE order_no=?";
                           Statement query(database, sql.c_str());
                           query.bind(1, orderNo);
                           return query.row() ? std::optional<ChargingOrder>(readOrder(query))
                                              : std::nullopt;
                       });
}

std::optional<ChargingOrder> SqliteRepository::orderByFlow(const std::string& flowNo)
{
    return useDatabase(this, databasePath_,
                       [&](sqlite3* database) -> std::optional<ChargingOrder>
                       {
                           const std::string sql = std::string("SELECT ") + orderColumns +
                                                   " FROM charging_order WHERE flow_no=?";
                           Statement query(database, sql.c_str());
                           query.bind(1, flowNo);
                           return query.row() ? std::optional<ChargingOrder>(readOrder(query))
                                              : std::nullopt;
                       });
}

std::vector<ChargingOrder> SqliteRepository::orders(const std::int64_t userId,
                                                    const std::optional<int> status,
                                                    const std::int64_t fromAt,
                                                    const std::int64_t toAt)
{
    return useDatabase(
        this, databasePath_,
        [&](sqlite3* database)
        {
            const std::string sql =
                std::string("SELECT ") + orderColumns +
                " FROM charging_order WHERE user_id=? AND (? IS NULL OR status=?) AND "
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

std::vector<UserAccount> SqliteRepository::listAccounts()
{
    return useDatabase(this, databasePath_,
                       [&](sqlite3* database)
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

bool SqliteRepository::addStation(Station& station)
{
    return useDatabase(this, databasePath_,
                       [&](sqlite3* database) -> bool
                       {
                           Statement exists(database, "SELECT 1 FROM station WHERE code=? LIMIT 1");
                           exists.bind(1, station.code);
                           if (exists.row())
                               return false;
                           Statement nextId(database, "SELECT COALESCE(MAX(id),0)+1 FROM station");
                           const std::int64_t assignedId = nextId.row() ? nextId.integer(0) : 1;
                           Statement insert(database,
                                            "INSERT INTO "
                                            "station(id,code,name,address,adcode,latitude_e6,"
                                            "longitude_e6,business_hours,enabled,version) "
                                            "VALUES(?,?,?,?,?,?,?,?,?,?)");
                           insert.bind(1, assignedId);
                           insert.bind(2, station.code);
                           insert.bind(3, station.name);
                           insert.bind(4, station.address);
                           insert.bind(5, station.adcode);
                           insert.bind(6, station.latitudeE6);
                           insert.bind(7, station.longitudeE6);
                           insert.bind(8, station.businessHours);
                           insert.bind(9, station.enabled ? 1 : 0);
                           insert.bind(10, station.version);
                           insert.execute();
                           station.id = assignedId;
                           return true;
                       });
}

bool SqliteRepository::saveStation(const Station& station)
{
    return useDatabase(this, databasePath_,
                       [&](sqlite3* database) -> bool
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
                           return sqlite3_changes(database) > 0;
                       });
}

bool SqliteRepository::stationCodeExists(const std::string& code)
{
    return useDatabase(this, databasePath_,
                       [&](sqlite3* database) -> bool
                       {
                           Statement query(database, "SELECT 1 FROM station WHERE code=? LIMIT 1");
                           query.bind(1, code);
                           return query.row();
                       });
}

bool SqliteRepository::addCharger(Charger& charger)
{
    return useDatabase(this, databasePath_,
                       [&](sqlite3* database) -> bool
                       {
                           Statement exists(database, "SELECT 1 FROM charger WHERE code=? LIMIT 1");
                           exists.bind(1, charger.code);
                           if (exists.row())
                               return false;
                           Statement nextId(database, "SELECT COALESCE(MAX(id),0)+1 FROM charger");
                           const std::int64_t assignedId = nextId.row() ? nextId.integer(0) : 1;
                           Statement insert(
                               database,
                               "INSERT INTO "
                               "charger(id,station_id,code,charger_type,power_watt,"
                               "connector_standard,status,total_count,total_minutes,version)"
                               " VALUES(?,?,?,?,?,?,?,?,?,?)");
                           insert.bind(1, assignedId);
                           insert.bind(2, charger.stationId);
                           insert.bind(3, charger.code);
                           insert.bind(4, static_cast<int>(charger.type));
                           insert.bind(5, charger.powerWatt);
                           insert.bind(6, charger.connectorStandard);
                           insert.bind(7, static_cast<int>(charger.status));
                           insert.bind(8, charger.totalCount);
                           insert.bind(9, charger.totalMinutes);
                           insert.bind(10, charger.version);
                           insert.execute();
                           charger.id = assignedId;
                           return true;
                       });
}

bool SqliteRepository::chargerCodeExists(const std::string& code)
{
    return useDatabase(this, databasePath_,
                       [&](sqlite3* database) -> bool
                       {
                           Statement query(database, "SELECT 1 FROM charger WHERE code=? LIMIT 1");
                           query.bind(1, code);
                           return query.row();
                       });
}

void SqliteRepository::addTariff(const RegionTariff& tariff)
{
    useDatabase(this, databasePath_,
                [&](sqlite3* database)
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

std::vector<RegionTariff> SqliteRepository::tariffVersions(std::optional<std::string> adcode)
{
    return useDatabase(
        this, databasePath_,
        [&](sqlite3* database)
        {
            Statement query(database, "SELECT "
                                      "adcode,electricity_cent_per_kwh,service_cent_per_kwh,"
                                      "effective_from,effective_to FROM region_tariff WHERE "
                                      "(? IS NULL OR adcode=?) ORDER BY effective_from");
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

std::vector<ChargingFlow> SqliteRepository::allFlows()
{
    return useDatabase(this, databasePath_,
                       [&](sqlite3* database)
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

std::vector<ChargingOrder> SqliteRepository::allOrders()
{
    return useDatabase(this, databasePath_,
                       [&](sqlite3* database)
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

ReadinessStatus SqliteRepository::check()
{
    std::lock_guard lock(readinessMutex_);
    return readiness_;
}

void SqliteRepository::refreshReadiness()
{
    const ReadinessStatus latest = probeDatabase();
    std::lock_guard lock(readinessMutex_);
    readiness_ = latest;
}

ReadinessStatus SqliteRepository::probeDatabase()
{
    ReadinessStatus status;
    try
    {
        Connection connection(databasePath_);
        Statement version(connection.get(), "SELECT version,checksum FROM schema_version ORDER BY "
                                            "version DESC LIMIT 1");
        status.schemaVersion = version.row() && version.integer(0) == kLatestSchemaVersion &&
                               version.text(1) == kLatestSchemaChecksum;
        Statement wal(connection.get(), "PRAGMA journal_mode");
        status.walEnabled = wal.row() && wal.text(0) == "wal";
        connection.execute("BEGIN IMMEDIATE");
        Statement touch(connection.get(), "UPDATE schema_version SET applied_at=applied_at "
                                          "WHERE version=?");
        touch.bind(1, static_cast<std::int64_t>(kLatestSchemaVersion));
        touch.execute();
        connection.execute("ROLLBACK");
        status.databaseReadWrite = true;
        status.migrationsComplete = status.schemaVersion;
    }
    catch (...)
    {
        return {};
    }
    return status;
}

std::int64_t SqliteRepository::nextBusinessSequence(const std::string_view prefix,
                                                    const std::int64_t utcDay)
{
    return useDatabase(this, databasePath_,
                       [&](sqlite3* database)
                       {
                           Statement statement(
                               database,
                               "INSERT INTO business_sequence(prefix,utc_day,value) VALUES(?,?,1) "
                               "ON CONFLICT(prefix,utc_day) DO UPDATE SET value=value+1 RETURNING "
                               "value");
                           statement.bind(1, prefix);
                           statement.bind(2, utcDay);
                           if (!statement.row())
                               throw std::runtime_error("business sequence unavailable");
                           return statement.integer(0);
                       });
}

std::optional<PersistedIdempotencyRecord>
SqliteRepository::loadIdempotencyRecord(const std::string_view scope, const std::string_view key)
{
    return useDatabase(this, databasePath_,
                       [&](sqlite3* database) -> std::optional<PersistedIdempotencyRecord>
                       {
                           Statement query(
                               database,
                               "SELECT "
                               "request_digest,result_status,result_content_type,result_body,"
                               "expires_at,lease_expires_at,lease_token,permanent "
                               "FROM idempotency_record WHERE scope=? AND idempotency_key=?");
                           query.bind(1, scope);
                           query.bind(2, key);
                           if (!query.row())
                               return std::nullopt;
                           PersistedIdempotencyRecord record;
                           record.scope = std::string(scope);
                           record.key = std::string(key);
                           record.requestDigest = query.text(0);
                           if (!query.isNull(1))
                           {
                               record.result = StoredHttpResult{static_cast<int>(query.integer(1)),
                                                                query.text(2), query.text(3)};
                           }
                           record.expiresAt = query.integer(4);
                           record.leaseExpiresAt = query.integer(5);
                           record.leaseToken = query.text(6);
                           record.permanent = query.integer(7) != 0;
                           return record;
                       });
}

void SqliteRepository::saveIdempotencyRecord(const PersistedIdempotencyRecord& record)
{
    useDatabase(this, databasePath_,
                [&](sqlite3* database)
                {
                    Statement statement(
                        database,
                        "INSERT INTO idempotency_record(scope,idempotency_key,request_digest,"
                        "result_status,result_content_type,result_body,expires_at,lease_"
                        "expires_at,"
                        "lease_token,permanent) VALUES(?,?,?,?,?,?,?,?,?,?) "
                        "ON CONFLICT(scope,idempotency_key) DO UPDATE SET "
                        "request_digest=excluded.request_digest,result_status=excluded.result_"
                        "status,"
                        "result_content_type=excluded.result_content_type,result_body=excluded."
                        "result_body,"
                        "expires_at=excluded.expires_at,lease_expires_at=excluded.lease_"
                        "expires_at,"
                        "lease_token=excluded.lease_token,permanent=excluded.permanent");
                    statement.bind(1, record.scope);
                    statement.bind(2, record.key);
                    statement.bind(3, record.requestDigest);
                    if (record.result)
                    {
                        statement.bind(4, record.result->status);
                        statement.bind(5, record.result->contentType);
                        statement.bind(6, record.result->body);
                    }
                    else
                    {
                        statement.bindNull(4);
                        statement.bindNull(5);
                        statement.bindNull(6);
                    }
                    statement.bind(7, record.expiresAt);
                    statement.bind(8, record.leaseExpiresAt);
                    statement.bind(9, record.leaseToken);
                    statement.bind(10, record.permanent ? 1 : 0);
                    statement.execute();
                });
}

void SqliteRepository::removeIdempotencyRecord(const std::string_view scope,
                                               const std::string_view key)
{
    useDatabase(this, databasePath_,
                [&](sqlite3* database)
                {
                    Statement statement(
                        database,
                        "DELETE FROM idempotency_record WHERE scope=? AND idempotency_key=?");
                    statement.bind(1, scope);
                    statement.bind(2, key);
                    statement.execute();
                });
}

void SqliteRepository::cleanupIdempotencyRecords(const std::int64_t now)
{
    useDatabase(this, databasePath_,
                [&](sqlite3* database)
                {
                    Statement statement(
                        database, "DELETE FROM idempotency_record WHERE "
                                  "(result_status IS NULL AND lease_expires_at<=?) OR "
                                  "(result_status IS NOT NULL AND permanent=0 AND expires_at<=?)");
                    statement.bind(1, now);
                    statement.bind(2, now);
                    statement.execute();
                });
}

std::size_t SqliteRepository::idempotencyRecordCount()
{
    return useDatabase(this, databasePath_,
                       [&](sqlite3* database)
                       {
                           Statement query(database, "SELECT count(*) FROM idempotency_record");
                           if (!query.row())
                               return std::size_t{0};
                           return static_cast<std::size_t>(query.integer(0));
                       });
}

void SqliteRepository::ensureDevelopmentAdmin(const bool enabled)
{
    if (!enabled)
    {
        useDatabase(this, databasePath_,
                    [](sqlite3* database)
                    {
                        Statement disable(database, "UPDATE admin_account SET status=0 "
                                                    "WHERE is_demo=1 AND status<>0");
                        disable.execute();
                    });
        return;
    }
    const bool exists = useDatabase(
        this, databasePath_,
        [](sqlite3* database)
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
                             "INSERT OR IGNORE INTO admin_account(username,password_hash,status,"
                             "must_change_password,is_demo,version) VALUES('admin',?,1,0,1,1)");
            insert.bind(1, passwordHash);
            insert.execute();
            Statement roles(transactionContext.database,
                            "INSERT OR IGNORE INTO admin_role(admin_id,role) "
                            "SELECT id,? FROM admin_account WHERE username='admin' AND is_demo=1");
            roles.bind(1, "OPERATOR");
            roles.execute();
            Statement owner(
                transactionContext.database,
                "INSERT OR IGNORE INTO admin_role(admin_id,role) "
                "SELECT id,'OWNER' FROM admin_account WHERE username='admin' AND is_demo=1");
            owner.execute();
        });
}

std::optional<AdminAccount> SqliteRepository::findAdminByUsername(const std::string_view username)
{
    return useDatabase(this, databasePath_,
                       [&](sqlite3* database) -> std::optional<AdminAccount>
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

std::optional<AdminAccount> SqliteRepository::findAdminById(const std::int64_t id)
{
    return useDatabase(
        this, databasePath_,
        [&](sqlite3* database) -> std::optional<AdminAccount>
        {
            Statement query(database, "SELECT id,username,password_hash,status,"
                                      "must_change_password,version FROM admin_account WHERE id=?");
            query.bind(1, id);
            return query.row() ? std::optional<AdminAccount>(readAdmin(database, query))
                               : std::nullopt;
        });
}

AdminUserPage SqliteRepository::listManagedUsers(const AdminUserQuery& query)
{
    return useDatabase(
        this, databasePath_,
        [&](sqlite3* database)
        {
            const std::string predicate =
                " FROM user_account WHERE deleted=0 AND (? IS NULL OR status=?) AND "
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

std::optional<UserAccount> SqliteRepository::findManagedUser(const std::int64_t id)
{
    return findById(id);
}

AccountWriteResult SqliteRepository::updateManagedUserStatus(
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
            if (sqlite3_changes(transactionContext.database) == 0)
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

void SqliteRepository::addAuditEvent(const AuditEvent& event)
{
    useDatabase(this, databasePath_,
                [&](sqlite3* database)
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

std::vector<AuditEvent> SqliteRepository::auditEvents(const AuditEventQuery& query)
{
    return useDatabase(
        this, databasePath_,
        [&](sqlite3* database)
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

bool SqliteRepository::createStationWithChargers(Station& station, const InitialChargerSpec& spec)
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

std::vector<Station> SqliteRepository::stations(const std::optional<int> status,
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

bool SqliteRepository::addChargers(std::vector<Charger>& chargers)
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

void SqliteRepository::addTariffVersion(const RegionTariff& tariff)
{
    addTariff(tariff);
}

bool SqliteRepository::tariffOverlaps(const std::string& adcode, const std::int64_t from,
                                      const std::int64_t to)
{
    return useDatabase(this, databasePath_,
                       [&](sqlite3* database)
                       {
                           Statement query(database,
                                           "SELECT 1 FROM region_tariff WHERE adcode=? AND "
                                           "?<=effective_to AND ?>=effective_from LIMIT 1");
                           query.bind(1, adcode);
                           query.bind(2, from);
                           query.bind(3, to);
                           return query.row();
                       });
}

std::int64_t SqliteRepository::addPriceAdjustment(const PriceAdjustment& adjustment)
{
    return useDatabase(this, databasePath_,
                       [&](sqlite3* database)
                       {
                           Statement insert(
                               database, "INSERT INTO price_adjustment(station_id,charger_type,"
                                         "source,adjustment_bp,effective_from,effective_to,reason) "
                                         "VALUES(?,?,?,?,?,?,?)");
                           insert.bind(1, adjustment.stationId);
                           insert.bind(2, adjustment.chargerType);
                           insert.bind(3, adjustment.source);
                           insert.bind(4, adjustment.adjustmentBp);
                           insert.bind(5, adjustment.effectiveFrom);
                           insert.bind(6, adjustment.effectiveTo);
                           insert.bind(7, adjustment.reason);
                           insert.execute();
                           return sqlite3_last_insert_rowid(database);
                       });
}

std::optional<PriceAdjustment>
SqliteRepository::effectivePriceAdjustment(const std::int64_t stationId, const int chargerType,
                                           const std::int64_t at)
{
    return useDatabase(
        this, databasePath_,
        [&](sqlite3* database) -> std::optional<PriceAdjustment>
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

void SqliteRepository::addDeviceCommand(const DeviceCommand& command)
{
    useDatabase(this, databasePath_,
                [&](sqlite3* database)
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

std::optional<DeviceCommand> SqliteRepository::deviceCommand(const std::string& commandNo)
{
    return useDatabase(
        this, databasePath_,
        [&](sqlite3* database) -> std::optional<DeviceCommand>
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

void SqliteRepository::saveDeviceCommand(const DeviceCommand& command)
{
    useDatabase(this, databasePath_,
                [&](sqlite3* database)
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

std::vector<DeviceCommand> SqliteRepository::dueDeviceCommands(const std::int64_t now)
{
    return useDatabase(
        this, databasePath_,
        [&](sqlite3* database)
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

std::optional<ChargingFlow> SqliteRepository::activeFlowOnCharger(const std::int64_t chargerId)
{
    return useDatabase(
        this, databasePath_,
        [&](sqlite3* database) -> std::optional<ChargingFlow>
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

bool SqliteRepository::stationHasActiveFlow(const std::int64_t stationId)
{
    return useDatabase(this, databasePath_,
                       [&](sqlite3* database)
                       {
                           Statement query(database,
                                           "SELECT 1 FROM charging_flow WHERE station_id=? AND "
                                           "status IN (10,20,30,40,50,80) LIMIT 1");
                           query.bind(1, stationId);
                           return query.row();
                       });
}

AdminFlowPage SqliteRepository::flows(const AdminFlowQuery& query)
{
    return useDatabase(
        this, databasePath_,
        [&](sqlite3* database)
        {
            const std::string predicate =
                " FROM charging_flow WHERE (? IS NULL OR status=?) AND "
                "(? IS NULL OR station_id=?) AND (? IS NULL OR charger_id=?) AND "
                "(? IS NULL OR user_id=?)";
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
SqliteRepository::settledOrders(const std::int64_t fromAt, const std::int64_t toAt,
                                const std::optional<std::int64_t> stationId)
{
    return useDatabase(
        this, databasePath_,
        [&](sqlite3* database)
        {
            const std::string sql =
                std::string("SELECT ") + orderColumns +
                " FROM charging_order WHERE status=? AND settled_at IS NOT NULL "
                "AND settled_at>=? AND settled_at<=? AND (? IS NULL OR station_id=?) "
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

std::optional<MlTask> SqliteRepository::runningMlTask(const std::string& taskType)
{
    return useDatabase(
        this, databasePath_,
        [&](sqlite3* database) -> std::optional<MlTask>
        {
            Statement query(database,
                            "SELECT task_no,task_type,status,model_version,horizon_hours,"
                            "created_at,finished_at,metrics_summary,error_summary FROM ml_task "
                            "WHERE task_type=? AND status IN ('PENDING','RUNNING') "
                            "ORDER BY created_at DESC LIMIT 1");
            query.bind(1, taskType);
            return query.row() ? std::optional<MlTask>(readMlTask(query)) : std::nullopt;
        });
}

std::vector<MlTask> SqliteRepository::overdueMlTasks(const std::int64_t trainDeadline,
                                                     const std::int64_t predictDeadline)
{
    return useDatabase(this, databasePath_,
                       [&](sqlite3* database)
                       {
                           Statement query(
                               database,
                               "SELECT task_no,task_type,status,model_version,horizon_hours,"
                               "created_at,finished_at,metrics_summary,error_summary FROM ml_task "
                               "WHERE status IN ('PENDING','RUNNING') AND "
                               "((task_type='TRAIN' AND created_at<=?) OR "
                               "(task_type='PREDICT' AND created_at<=?)) "
                               "ORDER BY created_at,task_no LIMIT 100");
                           query.bind(1, trainDeadline);
                           query.bind(2, predictDeadline);
                           std::vector<MlTask> tasks;
                           while (query.row())
                               tasks.push_back(readMlTask(query));
                           return tasks;
                       });
}

void SqliteRepository::addMlTask(const MlTask& task)
{
    useDatabase(this, databasePath_,
                [&](sqlite3* database)
                {
                    Statement insert(
                        database,
                        "INSERT INTO ml_task(task_no,task_type,status,model_version,"
                        "horizon_hours,created_at,finished_at,metrics_summary,error_summary) "
                        "VALUES(?,?,?,?,?,?,?,?,?)");
                    insert.bind(1, task.taskNo);
                    insert.bind(2, task.taskType);
                    insert.bind(3, task.status);
                    insert.bind(4, task.modelVersion);
                    insert.bind(5, encodeHorizons(task.horizonHours));
                    insert.bind(6, task.createdAt);
                    bindOptional(insert, 7, task.finishedAt);
                    insert.bind(8, task.metricsSummary);
                    insert.bind(9, task.errorSummary);
                    insert.execute();
                });
}

std::optional<MlTask> SqliteRepository::mlTask(const std::string& taskNo)
{
    return useDatabase(
        this, databasePath_,
        [&](sqlite3* database) -> std::optional<MlTask>
        {
            Statement query(database,
                            "SELECT task_no,task_type,status,model_version,horizon_hours,"
                            "created_at,finished_at,metrics_summary,error_summary FROM ml_task "
                            "WHERE task_no=?");
            query.bind(1, taskNo);
            return query.row() ? std::optional<MlTask>(readMlTask(query)) : std::nullopt;
        });
}

void SqliteRepository::saveMlTask(const MlTask& task)
{
    useDatabase(this, databasePath_,
                [&](sqlite3* database)
                {
                    Statement update(
                        database,
                        "UPDATE ml_task SET status=?,model_version=?,horizon_hours=?,"
                        "finished_at=?,metrics_summary=?,error_summary=? WHERE task_no=?");
                    update.bind(1, task.status);
                    update.bind(2, task.modelVersion);
                    update.bind(3, encodeHorizons(task.horizonHours));
                    bindOptional(update, 4, task.finishedAt);
                    update.bind(5, task.metricsSummary);
                    update.bind(6, task.errorSummary);
                    update.bind(7, task.taskNo);
                    update.execute();
                });
}

bool SqliteRepository::tryFinishMlTask(const MlTask& task, const bool allowTimedOut)
{
    return useDatabase(this, databasePath_,
                       [&](sqlite3* database)
                       {
                           const char* sql =
                               allowTimedOut ? "UPDATE ml_task SET status=?,model_version=?,"
                                               "horizon_hours=?,finished_at=?,metrics_summary=?,"
                                               "error_summary=? WHERE task_no=? AND status IN "
                                               "('PENDING','RUNNING','TIMED_OUT')"
                                             : "UPDATE ml_task SET status=?,model_version=?,"
                                               "horizon_hours=?,finished_at=?,metrics_summary=?,"
                                               "error_summary=? WHERE task_no=? AND status IN "
                                               "('PENDING','RUNNING')";
                           Statement update(database, sql);
                           update.bind(1, task.status);
                           update.bind(2, task.modelVersion);
                           update.bind(3, encodeHorizons(task.horizonHours));
                           bindOptional(update, 4, task.finishedAt);
                           update.bind(5, task.metricsSummary);
                           update.bind(6, task.errorSummary);
                           update.bind(7, task.taskNo);
                           update.execute();
                           return sqlite3_changes(database) == 1;
                       });
}

void SqliteRepository::addBackup(const BackupRecord& record)
{
    useDatabase(this, databasePath_,
                [&](sqlite3* database)
                {
                    Statement insert(
                        database, "INSERT INTO backup_record(backup_no,status,checksum,size_bytes,"
                                  "created_at,verification_status,verified_at,storage_path) "
                                  "VALUES(?,?,?,?,?,?,?,?)");
                    insert.bind(1, record.backupNo);
                    insert.bind(2, record.status);
                    insert.bind(3, record.checksum);
                    insert.bind(4, record.sizeBytes);
                    insert.bind(5, record.createdAt);
                    insert.bind(6, record.verificationStatus);
                    bindOptional(insert, 7, record.verifiedAt);
                    insert.bind(8, record.storagePath);
                    insert.execute();
                });
}

std::vector<BackupRecord> SqliteRepository::backups()
{
    return useDatabase(this, databasePath_,
                       [&](sqlite3* database)
                       {
                           Statement query(
                               database,
                               "SELECT backup_no,status,checksum,size_bytes,created_at,"
                               "verification_status,verified_at,storage_path FROM backup_record "
                               "ORDER BY created_at DESC,backup_no DESC");
                           std::vector<BackupRecord> records;
                           while (query.row())
                           {
                               BackupRecord record;
                               record.backupNo = query.text(0);
                               record.status = query.text(1);
                               record.checksum = query.text(2);
                               record.sizeBytes = query.integer(3);
                               record.createdAt = query.integer(4);
                               record.verificationStatus = query.text(5);
                               record.verifiedAt = optionalInteger(query, 6);
                               record.storagePath = query.text(7);
                               records.push_back(std::move(record));
                           }
                           return records;
                       });
}

std::optional<BackupRecord> SqliteRepository::backup(const std::string& backupNo)
{
    return useDatabase(this, databasePath_,
                       [&](sqlite3* database) -> std::optional<BackupRecord>
                       {
                           Statement query(
                               database,
                               "SELECT backup_no,status,checksum,size_bytes,created_at,"
                               "verification_status,verified_at,storage_path FROM backup_record "
                               "WHERE backup_no=?");
                           query.bind(1, backupNo);
                           if (!query.row())
                               return std::nullopt;
                           BackupRecord record;
                           record.backupNo = query.text(0);
                           record.status = query.text(1);
                           record.checksum = query.text(2);
                           record.sizeBytes = query.integer(3);
                           record.createdAt = query.integer(4);
                           record.verificationStatus = query.text(5);
                           record.verifiedAt = optionalInteger(query, 6);
                           record.storagePath = query.text(7);
                           return record;
                       });
}

void SqliteRepository::saveBackup(const BackupRecord& record)
{
    useDatabase(this, databasePath_,
                [&](sqlite3* database)
                {
                    Statement update(
                        database,
                        "UPDATE backup_record SET status=?,checksum=?,size_bytes=?,"
                        "verification_status=?,verified_at=?,storage_path=? WHERE backup_no=?");
                    update.bind(1, record.status);
                    update.bind(2, record.checksum);
                    update.bind(3, record.sizeBytes);
                    update.bind(4, record.verificationStatus);
                    bindOptional(update, 5, record.verifiedAt);
                    update.bind(6, record.storagePath);
                    update.bind(7, record.backupNo);
                    update.execute();
                });
}

bool SqliteRepository::createBackupSnapshot(BackupRecord& record)
{
    if (record.backupNo.empty() ||
        record.backupNo.find_first_not_of(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") !=
            std::string::npos)
    {
        return false;
    }
    const std::filesystem::path database(databasePath_);
    const std::filesystem::path directory =
        database.parent_path() / (database.filename().string() + ".backups");
    std::filesystem::create_directories(directory);
    if (!restrictOwnerPermissions(directory, true))
        return false;
    const std::filesystem::path destinationPath = directory / (record.backupNo + ".db");
    if (std::filesystem::exists(destinationPath))
        return false;

    sqlite3* destination = nullptr;
    if (sqlite3_open_v2(destinationPath.string().c_str(), &destination,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK)
    {
        if (destination)
            sqlite3_close(destination);
        std::error_code ignored;
        std::filesystem::remove(destinationPath, ignored);
        return false;
    }
    bool succeeded = false;
    try
    {
        // A backup invoked by an idempotent route runs inside a write transaction.
        // Use a distinct read connection so sqlite3_backup never reads from the
        // connection that currently owns that transaction.
        Connection sourceConnection(databasePath_);
        sqlite3* source = sourceConnection.get();
        sqlite3_backup* backupHandle = sqlite3_backup_init(destination, "main", source, "main");
        if (backupHandle)
        {
            int step = SQLITE_OK;
            int retries = 0;
            while (step == SQLITE_OK || step == SQLITE_BUSY || step == SQLITE_LOCKED)
            {
                step = sqlite3_backup_step(backupHandle, 256);
                if (step == SQLITE_BUSY || step == SQLITE_LOCKED)
                {
                    if (++retries > 100)
                        break;
                    sqlite3_sleep(10);
                }
            }
            const int finish = sqlite3_backup_finish(backupHandle);
            succeeded = (step == SQLITE_DONE && finish == SQLITE_OK);
        }
    }
    catch (...)
    {
        succeeded = false;
    }
    if (sqlite3_close(destination) != SQLITE_OK)
        succeeded = false;
    if (!succeeded)
    {
        std::error_code ignored;
        std::filesystem::remove(destinationPath, ignored);
        return false;
    }

    try
    {
        if (!restrictOwnerPermissions(destinationPath, false))
            throw std::runtime_error("backup permissions could not be restricted");
        record.storagePath = std::filesystem::absolute(destinationPath).string();
        record.sizeBytes = static_cast<std::int64_t>(std::filesystem::file_size(destinationPath));
        record.checksum = fileSha256(destinationPath);
        return record.sizeBytes > 0 && !record.checksum.empty();
    }
    catch (...)
    {
        std::error_code ignored;
        std::filesystem::remove(destinationPath, ignored);
        record.storagePath.clear();
        record.sizeBytes = 0;
        record.checksum.clear();
        return false;
    }
}

bool SqliteRepository::verifyBackupSnapshot(const BackupRecord& record)
{
    if (record.storagePath.empty() || record.checksum.empty() || record.sizeBytes <= 0)
        return false;
    const std::filesystem::path backupPath(record.storagePath);
    const std::filesystem::path allowedDirectory = std::filesystem::weakly_canonical(
        std::filesystem::path(databasePath_).parent_path() /
        (std::filesystem::path(databasePath_).filename().string() + ".backups"));
    std::error_code pathError;
    const std::filesystem::path canonicalBackup =
        std::filesystem::weakly_canonical(backupPath, pathError);
    if (pathError || canonicalBackup.parent_path() != allowedDirectory ||
        !std::filesystem::is_regular_file(canonicalBackup) ||
        static_cast<std::int64_t>(std::filesystem::file_size(canonicalBackup)) !=
            record.sizeBytes ||
        fileSha256(canonicalBackup) != record.checksum)
    {
        return false;
    }

    const std::filesystem::path temporary =
        allowedDirectory / (record.backupNo + "-verify-" + secureRandomToken(12) + ".db");
    try
    {
        std::filesystem::copy_file(canonicalBackup, temporary, std::filesystem::copy_options::none);
        Connection isolated(temporary.string());
        Statement integrity(isolated.get(), "PRAGMA integrity_check");
        const bool valid = integrity.row() && integrity.text(0) == "ok";
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return valid;
    }
    catch (...)
    {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
}

void SqliteRepository::cleanupAdminRecords(const std::int64_t now)
{
    constexpr std::int64_t retention = 180LL * 24 * 3600;
    constexpr std::int64_t deliveredOutboxRetention = 7LL * 24 * 3600;
    constexpr std::int64_t deadOutboxRetention = 30LL * 24 * 3600;
    useDatabase(
        this, databasePath_,
        [&](sqlite3* database)
        {
            Statement audit(database, "DELETE FROM ops_log WHERE at<?");
            audit.bind(1, now - retention);
            audit.execute();
            Statement commands(
                database,
                "DELETE FROM device_command WHERE completed_at IS NOT NULL AND completed_at<?");
            commands.bind(1, now - retention);
            commands.execute();
            Statement deliveredOutbox(database,
                                      "DELETE FROM outbox_event WHERE delivery_status=1 AND "
                                      "published_at IS NOT NULL AND published_at<?");
            deliveredOutbox.bind(1, now - deliveredOutboxRetention);
            deliveredOutbox.execute();
            Statement deadOutbox(
                database, "DELETE FROM outbox_event WHERE delivery_status=2 AND created_at<?");
            deadOutbox.bind(1, now - deadOutboxRetention);
            deadOutbox.execute();
        });
    pruneBackups();
}

void SqliteRepository::refreshHourlyMetrics(const std::int64_t fromAt, const std::int64_t toAt)
{
    if (fromAt < 0 || toAt <= fromAt)
        throw std::invalid_argument("invalid hourly metric range");
    const std::int64_t firstHour = fromAt / 3600 * 3600;
    // toAt is exclusive and only fully completed UTC hours are materialized.
    const std::int64_t exclusiveHour = toAt / 3600 * 3600;
    if (exclusiveHour <= firstHour)
        return;
    const std::int64_t lastHour = exclusiveHour - 3600;
    const std::int64_t refreshedAt = std::chrono::duration_cast<std::chrono::seconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count();
    // The rebuild is split into per-day UPSERT transactions. A single 90-day
    // DELETE+INSERT inside one BEGIN IMMEDIATE held the write lock long enough
    // to fail concurrent writers with SQLITE_BUSY; per-day chunks bound each
    // write-lock window, and UPSERT (instead of delete+insert) keeps every
    // bucket readable while the rebuild is in progress.
    constexpr std::int64_t kChunk = 24 * 3600;
    for (std::int64_t chunkStart = firstHour; chunkStart <= lastHour; chunkStart += kChunk)
    {
        const std::int64_t chunkEnd = std::min(chunkStart + kChunk - 3600, lastHour);
        withTransaction(
            [&]
            {
                useDatabase(
                    this, databasePath_,
                    [&](sqlite3* database)
                    {
                        Statement upsert(
                            database,
                            "WITH RECURSIVE hours(bucket_at) AS (SELECT ? UNION ALL "
                            "SELECT bucket_at+3600 FROM hours WHERE bucket_at+3600<=?), "
                            "aggregates AS (SELECT station_id,"
                            "(COALESCE(started_at,created_at)/3600)*3600 AS bucket_at,"
                            "SUM(energy_mwh) AS energy_mwh,COUNT(*) AS order_count,"
                            "SUM(CASE WHEN charger_type=1 THEN 1 ELSE 0 END) AS fast_count,"
                            "SUM(CASE WHEN charger_type=0 THEN 1 ELSE 0 END) AS slow_count,"
                            "SUM(CASE WHEN ended_at IS NOT NULL AND started_at IS NOT NULL "
                            "THEN MAX(0,ended_at-started_at) ELSE 0 END) AS busy_seconds "
                            "FROM charging_order WHERE status=? AND "
                            "COALESCE(started_at,created_at)>=? AND "
                            "COALESCE(started_at,created_at)<? GROUP BY station_id,bucket_at) "
                            "INSERT INTO station_hourly_metric(station_id,bucket_at,energy_mwh,"
                            "order_count,fast_order_count,slow_order_count,busy_device_seconds,"
                            "refreshed_at) SELECT s.id,h.bucket_at,COALESCE(a.energy_mwh,0),"
                            "COALESCE(a.order_count,0),COALESCE(a.fast_count,0),"
                            "COALESCE(a.slow_count,0),COALESCE(a.busy_seconds,0),? "
                            "FROM station s CROSS JOIN hours h LEFT JOIN aggregates a ON "
                            "a.station_id=s.id AND a.bucket_at=h.bucket_at "
                            "ON CONFLICT(station_id,bucket_at) DO UPDATE SET "
                            "energy_mwh=excluded.energy_mwh,order_count=excluded.order_count,"
                            "fast_order_count=excluded.fast_order_count,"
                            "slow_order_count=excluded.slow_order_count,"
                            "busy_device_seconds=excluded.busy_device_seconds,"
                            "refreshed_at=excluded.refreshed_at");
                        upsert.bind(1, chunkStart);
                        upsert.bind(2, chunkEnd);
                        upsert.bind(3, static_cast<int>(FlowStatus::Completed));
                        upsert.bind(4, chunkStart);
                        upsert.bind(5, chunkEnd + 3600);
                        upsert.bind(6, refreshedAt);
                        upsert.execute();
                    });
            });
    }
}

HourlyMetricPage SqliteRepository::hourlyMetrics(const std::int64_t fromAt, const std::int64_t toAt,
                                                 const std::optional<std::int64_t> stationId,
                                                 const std::string_view cursor, const int limit)
{
    std::int64_t cursorBucket = -1;
    std::int64_t cursorStation = -1;
    if (!cursor.empty())
    {
        const auto separator = cursor.find(':');
        if (separator == std::string_view::npos)
            throw std::invalid_argument("invalid metric cursor");
        const auto left = cursor.substr(0, separator);
        const auto right = cursor.substr(separator + 1);
        const auto first = std::from_chars(left.data(), left.data() + left.size(), cursorBucket);
        const auto second =
            std::from_chars(right.data(), right.data() + right.size(), cursorStation);
        if (first.ec != std::errc{} || first.ptr != left.data() + left.size() ||
            second.ec != std::errc{} || second.ptr != right.data() + right.size() ||
            cursorBucket < 0 || cursorStation < 0)
            throw std::invalid_argument("invalid metric cursor");
    }
    return useDatabase(
        this, databasePath_,
        [&](sqlite3* database)
        {
            Statement query(
                database,
                "SELECT m.station_id,s.name,m.bucket_at,m.energy_mwh,m.order_count,"
                "m.fast_order_count,m.slow_order_count,"
                "m.busy_device_seconds,"
                "(SELECT COUNT(*) FROM charger c WHERE c.station_id=m.station_id "
                "AND c.status IN (0,1)) "
                "FROM station_hourly_metric m JOIN station s ON s.id=m.station_id "
                "WHERE m.bucket_at>=? AND m.bucket_at<=? AND (? IS NULL OR m.station_id=?) "
                "AND (m.bucket_at>? OR (m.bucket_at=? AND m.station_id>?)) "
                "ORDER BY m.bucket_at,m.station_id LIMIT ?");
            query.bind(1, fromAt / 3600 * 3600);
            query.bind(2, toAt / 3600 * 3600);
            if (stationId)
            {
                query.bind(3, *stationId);
                query.bind(4, *stationId);
            }
            else
            {
                query.bindNull(3);
                query.bindNull(4);
            }
            query.bind(5, cursorBucket);
            query.bind(6, cursorBucket);
            query.bind(7, cursorStation);
            query.bind(8, limit + 1);
            HourlyMetricPage page;
            while (query.row())
            {
                page.items.push_back(HourlyMetric{
                    query.integer(0), query.text(1), query.integer(2), query.integer(3),
                    static_cast<int>(query.integer(4)), static_cast<int>(query.integer(5)),
                    static_cast<int>(query.integer(6)), static_cast<int>(query.integer(8)),
                    query.integer(7)});
            }
            if (page.items.size() > static_cast<std::size_t>(limit))
            {
                page.items.resize(static_cast<std::size_t>(limit));
                const auto& last = page.items.back();
                page.nextCursor =
                    std::to_string(last.bucketAt) + ":" + std::to_string(last.stationId);
            }
            return page;
        });
}

std::int64_t SqliteRepository::nextDashboardVersion()
{
    return useDatabase(this, databasePath_,
                       [&](sqlite3* database)
                       {
                           Statement update(
                               database, "UPDATE dashboard_state SET data_version=data_version+1 "
                                         "WHERE singleton=1 RETURNING data_version");
                           if (!update.row())
                               throw std::runtime_error("dashboard version unavailable");
                           return update.integer(0);
                       });
}

void SqliteRepository::addModelVersion(const ModelVersion& version)
{
    useDatabase(this, databasePath_,
                [&](sqlite3* database)
                {
                    Statement insert(
                        database,
                        "INSERT INTO model_version(version_no,task_no,algorithm,"
                        "feature_schema_version,random_seed,train_from_at,train_to_at,mae,rmse,"
                        "mape,wape,baseline_mae,baseline_rmse,excluded_sample_count,qualified,"
                        "artifact_checksum,artifact_path,created_at) "
                        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
                    insert.bind(1, version.versionNo);
                    insert.bind(2, version.taskNo);
                    insert.bind(3, version.algorithm);
                    insert.bind(4, version.featureSchemaVersion);
                    insert.bind(5, version.randomSeed);
                    insert.bind(6, version.trainFromAt);
                    insert.bind(7, version.trainToAt);
                    insert.bind(8, version.mae);
                    insert.bind(9, version.rmse);
                    insert.bind(10, version.mape);
                    insert.bind(11, version.wape);
                    insert.bind(12, version.baselineMae);
                    insert.bind(13, version.baselineRmse);
                    insert.bind(14, version.excludedSampleCount);
                    insert.bind(15, version.qualified ? 1 : 0);
                    insert.bind(16, version.artifactChecksum);
                    insert.bind(17, version.artifactPath);
                    insert.bind(18, version.createdAt);
                    insert.execute();
                });
}

std::optional<ModelVersion> SqliteRepository::modelVersion(const std::string_view versionNo)
{
    return useDatabase(
        this, databasePath_,
        [&](sqlite3* database) -> std::optional<ModelVersion>
        {
            Statement query(
                database, "SELECT version_no,task_no,algorithm,feature_schema_version,random_seed,"
                          "train_from_at,train_to_at,mae,rmse,mape,wape,baseline_mae,"
                          "baseline_rmse,excluded_sample_count,qualified,artifact_checksum,"
                          "artifact_path,created_at FROM model_version WHERE version_no=?");
            query.bind(1, versionNo);
            if (!query.row())
                return std::nullopt;
            return ModelVersion{query.text(0),          query.text(1),
                                query.text(2),          static_cast<int>(query.integer(3)),
                                query.integer(4),       query.integer(5),
                                query.integer(6),       query.real(7),
                                query.real(8),          query.real(9),
                                query.real(10),         query.real(11),
                                query.real(12),         static_cast<int>(query.integer(13)),
                                query.integer(14) != 0, query.text(15),
                                query.text(16),         query.integer(17)};
        });
}

std::optional<ModelVersion> SqliteRepository::latestQualifiedModel()
{
    return useDatabase(
        this, databasePath_,
        [&](sqlite3* database) -> std::optional<ModelVersion>
        {
            Statement versionNo(
                database, "SELECT v.version_no FROM model_version v JOIN ml_task t "
                          "ON t.task_no=v.task_no WHERE v.qualified=1 AND t.status='SUCCEEDED' "
                          "ORDER BY v.created_at DESC,v.version_no DESC LIMIT 1");
            if (!versionNo.row())
                return std::nullopt;
            return modelVersion(versionNo.text(0));
        });
}

void SqliteRepository::upsertPredictions(const std::vector<LoadPrediction>& items)
{
    withTransaction(
        [&]
        {
            useDatabase(
                this, databasePath_,
                [&](sqlite3* database)
                {
                    Statement upsert(
                        database,
                        "INSERT INTO load_prediction(station_id,model_version_no,generated_at,"
                        "target_at,horizon_hour,predicted_energy_mwh,predicted_free_count,"
                        "is_peak,stale) VALUES(?,?,?,?,?,?,?,?,?) ON CONFLICT(station_id,"
                        "model_version_no,target_at) DO UPDATE SET "
                        "generated_at=excluded.generated_at,"
                        "horizon_hour=excluded.horizon_hour,predicted_energy_mwh=excluded."
                        "predicted_energy_mwh,"
                        "predicted_free_count=excluded.predicted_free_count,is_peak=excluded.is_"
                        "peak,stale=excluded.stale");
                    for (const auto& item : items)
                    {
                        upsert.bind(1, item.stationId);
                        upsert.bind(2, item.modelVersionNo);
                        upsert.bind(3, item.generatedAt);
                        upsert.bind(4, item.targetAt);
                        upsert.bind(5, item.horizonHour);
                        upsert.bind(6, item.predictedEnergyMwh);
                        upsert.bind(7, item.predictedFreeCount);
                        upsert.bind(8, item.isPeak ? 1 : 0);
                        upsert.bind(9, item.stale ? 1 : 0);
                        upsert.execute();
                        upsert.reset();
                    }
                });
        });
}

std::vector<LoadPrediction>
SqliteRepository::predictions(const std::optional<std::int64_t> stationId,
                              const std::optional<int> horizonHour, const std::int64_t fromAt)
{
    return useDatabase(
        this, databasePath_,
        [&](sqlite3* database)
        {
            Statement query(
                database, "SELECT station_id,horizon_hour,model_version_no,generated_at,target_at,"
                          "predicted_energy_mwh,predicted_free_count,is_peak,stale "
                          "FROM load_prediction WHERE target_at>=? AND "
                          "(? IS NULL OR station_id=?) AND (? IS NULL OR horizon_hour=?) "
                          "AND NOT EXISTS (SELECT 1 FROM load_prediction newer WHERE "
                          "newer.station_id=load_prediction.station_id AND "
                          "newer.target_at=load_prediction.target_at AND "
                          "(newer.generated_at>load_prediction.generated_at OR "
                          "(newer.generated_at=load_prediction.generated_at AND "
                          "newer.model_version_no>load_prediction.model_version_no))) "
                          "ORDER BY target_at,station_id,generated_at DESC");
            query.bind(1, fromAt);
            if (stationId)
            {
                query.bind(2, *stationId);
                query.bind(3, *stationId);
            }
            else
            {
                query.bindNull(2);
                query.bindNull(3);
            }
            if (horizonHour)
            {
                query.bind(4, *horizonHour);
                query.bind(5, *horizonHour);
            }
            else
            {
                query.bindNull(4);
                query.bindNull(5);
            }
            std::vector<LoadPrediction> values;
            while (query.row())
            {
                values.push_back(
                    LoadPrediction{query.integer(0), static_cast<int>(query.integer(1)),
                                   query.text(2), query.integer(3), query.integer(4),
                                   query.integer(5), static_cast<int>(query.integer(6)),
                                   query.integer(7) != 0, query.integer(8) != 0});
            }
            return values;
        });
}

void SqliteRepository::markPredictionsStale()
{
    useDatabase(this, databasePath_,
                [&](sqlite3* database)
                {
                    Statement update(database, "UPDATE load_prediction SET stale=1 WHERE stale=0");
                    update.execute();
                });
}

void SqliteRepository::cleanupAnalytics(const std::int64_t now)
{
    constexpr std::int64_t day = 24 * 3600;
    useDatabase(
        this, databasePath_,
        [&](sqlite3* database)
        {
            Statement predictions(database, "DELETE FROM load_prediction WHERE target_at<?");
            predictions.bind(1, now - 90 * day);
            predictions.execute();
            Statement metrics(database, "DELETE FROM station_hourly_metric WHERE bucket_at<?");
            metrics.bind(1, now - 365 * day);
            metrics.execute();
            // Keep metadata for models still referenced by retained predictions.
            Statement models(database,
                             "DELETE FROM model_version WHERE created_at<? AND version_no NOT IN "
                             "(SELECT DISTINCT model_version_no FROM load_prediction)");
            models.bind(1, now - 30 * day);
            models.execute();
        });
}

void SqliteRepository::pruneBackups()
{
    // NFR-R-03: keep the newest backup of each of the last seven days plus the
    // newest of each of the last four weeks; failed diagnostics age out after a
    // week. Files are removed through the same restricted directory check used
    // by verification so a corrupted record cannot delete arbitrary paths.
    const auto records = backups();
    constexpr std::int64_t day = 24 * 3600;
    const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
    const std::filesystem::path allowedDirectory = std::filesystem::weakly_canonical(
        std::filesystem::path(databasePath_).parent_path() /
        (std::filesystem::path(databasePath_).filename().string() + ".backups"));
    std::set<std::int64_t> keptDays;
    std::set<std::int64_t> keptWeeks;
    for (const auto& record : records)
    {
        bool keep = false;
        if (record.status == "SUCCEEDED")
        {
            const std::int64_t dayBucket = record.createdAt / day;
            const std::int64_t weekBucket = record.createdAt / (7 * day);
            if (keptDays.count(dayBucket) == 0 && keptDays.size() < 7)
            {
                keptDays.insert(dayBucket);
                keptWeeks.insert(weekBucket);
                keep = true;
            }
            else if (keptWeeks.count(weekBucket) == 0 && keptWeeks.size() < 4)
            {
                keptWeeks.insert(weekBucket);
                keep = true;
            }
        }
        else if (record.createdAt >= now - 7 * day)
        {
            keep = true;
        }
        if (keep)
            continue;
        if (!record.storagePath.empty() &&
            record.backupNo.find_first_not_of(
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_"
                "-") == std::string::npos)
        {
            std::error_code pathError;
            const std::filesystem::path candidate = std::filesystem::weakly_canonical(
                std::filesystem::path(record.storagePath), pathError);
            if (!pathError && candidate.parent_path() == allowedDirectory &&
                candidate.filename() == record.backupNo + ".db")
            {
                std::error_code ignored;
                std::filesystem::remove(candidate, ignored);
            }
        }
        useDatabase(this, databasePath_,
                    [&](sqlite3* database)
                    {
                        Statement remove(database, "DELETE FROM backup_record WHERE backup_no=?");
                        remove.bind(1, record.backupNo);
                        remove.execute();
                    });
    }
}

// UC-A-09 管理员账号管理: newest-first listing plus create / status /
// password mutations. Every mutation bumps the resource version, writes its
// audit event in the same transaction and is idempotent under retry except
// that a concurrent write wins the compare-and-swap.

AdminAccountPage SqliteRepository::adminAccounts(const AdminAccountQuery& query)
{
    return useDatabase(
        this, databasePath_,
        [&](sqlite3* database)
        {
            Statement count(database, "SELECT COUNT(*) FROM admin_account");
            AdminAccountPage page;
            page.page = query.page;
            page.pageSize = query.pageSize;
            if (count.row())
                page.total = static_cast<int>(count.integer(0));
            Statement select(database,
                             "SELECT id,username,password_hash,status,must_change_password,version "
                             "FROM admin_account ORDER BY id DESC LIMIT ? OFFSET ?");
            select.bind(1, query.pageSize);
            select.bind(2, static_cast<std::int64_t>(query.page - 1) * query.pageSize);
            while (select.row())
                page.items.push_back(readAdmin(database, select));
            return page;
        });
}

AdminAccountWriteResult SqliteRepository::createAdminAccount(const std::int64_t actorAdminId,
                                                             const std::string_view username,
                                                             const std::string_view passwordHash,
                                                             const std::string_view reason,
                                                             const std::int64_t at,
                                                             AdminAccount& created)
{
    try
    {
        withTransaction(
            [&]
            {
                Statement insert(transactionContext.database,
                                 "INSERT INTO admin_account(username,password_hash,status,"
                                 "must_change_password,is_demo,version) VALUES(?,?,1,1,0,1)");
                insert.bind(1, username);
                insert.bind(2, passwordHash);
                insert.execute();
                created.id = sqlite3_last_insert_rowid(transactionContext.database);
                created.username = std::string(username);
                created.passwordHash = std::string(passwordHash);
                created.status = 1;
                created.roles = {Role::Operator};
                created.mustChangePassword = true;
                created.version = 1;
                Statement role(transactionContext.database,
                               "INSERT INTO admin_role(admin_id,role) VALUES(?,?)");
                role.bind(1, created.id);
                role.bind(2, "OPERATOR");
                role.execute();
                addAuditEvent(AuditEvent{actorAdminId, "ADMIN_CREATED", "ADMIN",
                                         std::to_string(created.id), std::string(reason), at});
            });
        return AdminAccountWriteResult::Success;
    }
    catch (const std::exception&)
    {
        if (findAdminByUsername(username))
            return AdminAccountWriteResult::UsernameExists;
        throw;
    }
}

std::optional<AdminAccount>
SqliteRepository::bootstrapOwnerAccount(const std::string_view username,
                                        const std::string_view passwordHash, const std::int64_t at)
{
    AdminAccount created;
    withTransaction(
        [&]
        {
            // A demo OWNER (is_demo=1, disabled in production) never blocks the
            // bootstrap; only a real, non-demo OWNER makes it one-shot.
            Statement existing(transactionContext.database,
                               "SELECT 1 FROM admin_account a JOIN admin_role r ON "
                               "r.admin_id=a.id WHERE r.role='OWNER' AND a.is_demo=0");
            if (existing.row())
            {
                created.id = 0;
                return;
            }
            Statement usernameTaken(transactionContext.database,
                                    "SELECT 1 FROM admin_account WHERE username=?");
            usernameTaken.bind(1, username);
            if (usernameTaken.row())
                throw std::runtime_error("bootstrap owner username already exists");
            Statement insert(transactionContext.database,
                             "INSERT INTO admin_account(username,password_hash,status,"
                             "must_change_password,is_demo,version) VALUES(?,?,1,1,0,1)");
            insert.bind(1, username);
            insert.bind(2, passwordHash);
            insert.execute();
            created.id = sqlite3_last_insert_rowid(transactionContext.database);
            created.username = std::string(username);
            created.passwordHash = std::string(passwordHash);
            created.status = 1;
            created.roles = {Role::Owner};
            created.mustChangePassword = true;
            created.version = 1;
            Statement role(transactionContext.database,
                           "INSERT INTO admin_role(admin_id,role) VALUES(?,?)");
            role.bind(1, created.id);
            role.bind(2, "OWNER");
            role.execute();
            addAuditEvent(AuditEvent{created.id, "ADMIN_CREATED", "ADMIN",
                                     std::to_string(created.id), "bootstrap-owner", at});
        });
    return created.id == 0 ? std::nullopt : std::optional<AdminAccount>(std::move(created));
}

AdminAccountWriteResult SqliteRepository::updateAdminAccountStatus(
    const std::int64_t actorAdminId, const std::int64_t adminId, const int status,
    const std::string_view reason, const std::int64_t expectedVersion, const std::int64_t at,
    AdminAccount& updated)
{
    AdminAccountWriteResult result = AdminAccountWriteResult::NotFound;
    withTransaction(
        [&]
        {
            Statement update(transactionContext.database,
                             "UPDATE admin_account SET status=?,version=version+1 "
                             "WHERE id=? AND version=?");
            update.bind(1, status);
            update.bind(2, adminId);
            update.bind(3, expectedVersion);
            update.execute();
            if (sqlite3_changes(transactionContext.database) == 0)
            {
                const auto current = findAdminById(adminId);
                result = current ? AdminAccountWriteResult::VersionConflict
                                 : AdminAccountWriteResult::NotFound;
                return;
            }
            updated = *findAdminById(adminId);
            addAuditEvent(AuditEvent{actorAdminId, status == 0 ? "ADMIN_DISABLED" : "ADMIN_ENABLED",
                                     "ADMIN", std::to_string(adminId), std::string(reason), at});
            result = AdminAccountWriteResult::Success;
        });
    return result;
}

AdminAccountWriteResult SqliteRepository::changeAdminAccountPassword(
    const std::int64_t actorAdminId, const std::int64_t adminId,
    const std::string_view expectedCurrentHash, const std::string_view newPasswordHash,
    const std::int64_t at, AdminAccount& updated)
{
    AdminAccountWriteResult result = AdminAccountWriteResult::NotFound;
    withTransaction(
        [&]
        {
            Statement update(transactionContext.database,
                             "UPDATE admin_account SET password_hash=?,must_change_password=0,"
                             "version=version+1 WHERE id=? AND password_hash=?");
            update.bind(1, newPasswordHash);
            update.bind(2, adminId);
            update.bind(3, expectedCurrentHash);
            update.execute();
            if (sqlite3_changes(transactionContext.database) == 0)
            {
                const auto current = findAdminById(adminId);
                result = !current ? AdminAccountWriteResult::NotFound
                                  : AdminAccountWriteResult::HashMismatch;
                return;
            }
            updated = *findAdminById(adminId);
            addAuditEvent(AuditEvent{
                actorAdminId, "ADMIN_PASSWORD_CHANGED", "ADMIN", std::to_string(adminId), {}, at});
            result = AdminAccountWriteResult::Success;
        });
    return result;
}

} // namespace ncs::infrastructure::sqlite
