#include "core/application/business_numbers.h"
#include "core/application/wallet_service.h"
#include "infrastructure/sqlite/sqlite_repository.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <sqlite3.h>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace
{

std::int64_t processId()
{
#if defined(_WIN32)
    return static_cast<std::int64_t>(::_getpid());
#else
    return static_cast<std::int64_t>(::getpid());
#endif
}

class TestRunner final
{
  public:
    void check(const bool condition, const std::string_view message)
    {
        if (!condition)
        {
            std::cerr << "FAIL: " << message << '\n';
            ++failures_;
        }
    }
    int result() const
    {
        return failures_ == 0 ? 0 : 1;
    }

  private:
    int failures_ = 0;
};

class TemporaryDatabase final
{
  public:
    TemporaryDatabase()
        : path_(std::filesystem::temp_directory_path() /
                ("ncs-sqlite-seed-" + std::to_string(processId()) + ".db"))
    {
        cleanup();
    }

    ~TemporaryDatabase()
    {
        cleanup();
    }

    std::string path() const
    {
        return path_.string();
    }

  private:
    void cleanup() const
    {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(path_.string() + "-wal", ignored);
        std::filesystem::remove(path_.string() + "-shm", ignored);
    }

    std::filesystem::path path_;
};

class DatabaseReader final
{
  public:
    explicit DatabaseReader(std::string path) : path_(std::move(path)) {}

    std::int64_t integer(const std::string& sql) const
    {
        const auto rows = rowsOf(sql);
        return rows.empty() || rows.front().empty() ? -1 : std::stoll(rows.front());
    }

    // Rows whose single column is rendered as "a|b|c".
    std::vector<std::string> rows(const std::string& sql) const
    {
        return rowsOf(sql);
    }

  private:
    std::vector<std::string> rowsOf(const std::string& sql) const
    {
        sqlite3* database = nullptr;
        if (sqlite3_open_v2(path_.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        {
            throw std::runtime_error("test database open failed");
        }
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
        {
            const std::string message = sqlite3_errmsg(database);
            sqlite3_close(database);
            throw std::runtime_error(message);
        }
        std::vector<std::string> values;
        while (sqlite3_step(statement) == SQLITE_ROW)
        {
            const auto* cell = sqlite3_column_text(statement, 0);
            const int size = sqlite3_column_bytes(statement, 0);
            values.push_back(cell ? std::string(reinterpret_cast<const char*>(cell), size)
                                  : std::string());
        }
        sqlite3_finalize(statement);
        sqlite3_close(database);
        return values;
    }

    std::string path_;
};

void executeSql(const std::string& path, const std::string& sql)
{
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK)
    {
        throw std::runtime_error("test database open failed");
    }
    char* error = nullptr;
    const int result = sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &error);
    const std::string message = error ? error : "test SQL failed";
    sqlite3_free(error);
    sqlite3_close(database);
    if (result != SQLITE_OK)
        throw std::runtime_error(message);
}

std::vector<std::int64_t> integers(const std::string& text)
{
    std::vector<std::int64_t> values;
    std::size_t begin = 0;
    while (begin <= text.size())
    {
        const std::size_t end = text.find('|', begin);
        values.push_back(std::stoll(text.substr(begin, end - begin)));
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return values;
}

std::string unseed(const std::string& db)
{
    std::string sql = "DELETE FROM flow_event WHERE flow_no IN (SELECT flow_no FROM "
                      "charging_flow WHERE user_id IN (SELECT id FROM user_account WHERE "
                      "username LIKE 'sim_owner_%'));"
                      "DELETE FROM charging_order WHERE user_id IN (SELECT id FROM user_account "
                      "WHERE username LIKE 'sim_owner_%');"
                      "DELETE FROM charging_flow WHERE user_id IN (SELECT id FROM user_account "
                      "WHERE username LIKE 'sim_owner_%');"
                      "DELETE FROM wallet_transaction WHERE user_id IN (SELECT id FROM "
                      "user_account WHERE username LIKE 'sim_owner_%');"
                      "DELETE FROM recharge_order WHERE user_id IN (SELECT id FROM user_account "
                      "WHERE username LIKE 'sim_owner_%');"
                      "DELETE FROM wallet_account WHERE user_id IN (SELECT id FROM user_account "
                      "WHERE username LIKE 'sim_owner_%');"
                      "DELETE FROM user_account WHERE username LIKE 'sim_owner_%';"
                      "DELETE FROM charger WHERE station_id IN (SELECT id FROM station WHERE "
                      "code IN ('CYGY','BJN','SJS','TZYH')) OR code IN ('ZGC-DC-04','ZGC-DC-05',"
                      "'ZGC-DC-06','ZGC-AC-03','ZGC-AC-04');"
                      "DELETE FROM station WHERE code IN ('CYGY','BJN','SJS','TZYH');"
                      "DELETE FROM region_tariff WHERE adcode IN ('110106','110107','110112');"
                      "UPDATE region_tariff SET electricity_cent_per_kwh=92,"
                      "service_cent_per_kwh=48 WHERE adcode='110105' AND "
                      "electricity_cent_per_kwh=90;"
                      "INSERT INTO station(id,code,name,address,adcode,latitude_e6,"
                      "longitude_e6,business_hours,enabled) VALUES "
                      "(2,'XEQ','NCS 西二旗充电站','北京市海淀区上地信息路 2 号','110108',"
                      "40052768,116307517,'00:00-24:00',1),"
                      "(3,'CBD','NCS 国贸充电站','北京市朝阳区建国门外大街 1 号','110105',"
                      "39908372,116457658,'00:00-24:00',1);"
                      "INSERT INTO charger(id,station_id,code,charger_type,power_watt,"
                      "connector_standard,status,total_count,total_minutes) VALUES "
                      "(6,2,'XEQ-DC-01',1,120000,'GB/T',0,0,0),(7,2,'XEQ-AC-01',0,7000,'GB/T',"
                      "0,0,0),(8,3,'CBD-DC-01',1,60000,'GB/T',2,0,0),(9,3,'CBD-AC-01',0,7000,"
                      "'GB/T',0,0,0);"
                      "DELETE FROM schema_version WHERE version=8;";
    executeSql(db, sql);
    return db;
}

void checkIntegerIn(TestRunner& tests, DatabaseReader& reader, const std::string& sql,
                    const std::int64_t low, const std::int64_t high, const std::string_view message)
{
    const std::int64_t value = reader.integer(sql);
    tests.check(value >= low && value <= high, message);
}

} // namespace

int main()
{
    using namespace ncs::core::application;
    using ncs::infrastructure::sqlite::SqliteRepository;

    TestRunner tests;

    {
        // UC-D-02 fresh database: five SRS stations, 48 chargers, five-zone
        // tariffs, 300 owners and a full 90-day historical economy.
        TemporaryDatabase database;
        const std::string& db = database.path();
        {
            SqliteRepository repository(db);
            tests.check(repository.check().ready(), "seeded database is migrated and writable");
        }
        DatabaseReader reader(db);

        tests.check(reader.integer("SELECT MAX(version) FROM schema_version") == 8 &&
                        reader.rows("SELECT name||'|'||checksum FROM schema_version WHERE "
                                    "version=8")
                                .front() == "full-demo-seed|ncs-v8-full-demo-seed" &&
                        reader.integer("SELECT COUNT(*) FROM schema_version") == 8 &&
                        reader.integer("SELECT COUNT(*) FROM outbox_event") == 0,
                    "v8 full-demo-seed migration row exists once with no outbox events");
        checkIntegerIn(tests, reader,
                       "SELECT ABS(applied_at-strftime('%s','now')) FROM schema_version WHERE "
                       "version=8",
                       0, 3600, "v8 anchor time is the current first-run moment");
        const std::int64_t windowBegin =
            reader.integer("SELECT (applied_at/86400)*86400 FROM schema_version WHERE version=8") -
            90 * 86400;

        const std::vector<std::string> stations =
            reader.rows("SELECT code||'|'||name||'|'||address||'|'||adcode||'|'||latitude_e6||'|'||"
                        "longitude_e6 FROM station ORDER BY code");
        // Both expected lists below follow the ORDER BY code / sorted order the
        // queries below return them in.
        const std::vector<std::string> expectedStations{
            "BJN|NCS 北京南站充电站|北京市丰台区北京南站南广场|110106|39858897|116410717",
            "CYGY|NCS 朝阳公园充电站|北京市朝阳区朝阳公园南路 1 号|110105|39933660|116480863",
            "SJS|NCS 石景山充电站|北京市石景山区石景山路 68 号|110107|39923461|116150611",
            "TZYH|NCS 通州运河充电站|北京市通州区通胡大街 70 号|110112|39910655|116679698",
            "ZGC|NCS 中关村充电站|北京市海淀区中关村大街 27 号|110108|39977680|116316417"};
        tests.check(stations == expectedStations,
                    "exactly the five SRS stations exist with fixed attributes");
        tests.check(reader.integer("SELECT COUNT(*) FROM charger") == 48 &&
                        reader.integer("SELECT COUNT(*) FROM station") == 5 &&
                        reader.integer("SELECT COUNT(*) FROM charger WHERE status=2") == 6 &&
                        reader.integer("SELECT COUNT(*) FROM charger WHERE status=0") == 42,
                    "48 chargers with exactly six faulty and the rest idle");

        std::vector<std::string> histogram = reader.rows(
            "SELECT s.code||'|'||c.charger_type||'|'||c.power_watt||'|'||COUNT(*) FROM charger c "
            "JOIN station s ON s.id=c.station_id GROUP BY s.code,c.charger_type,c.power_watt");
        const std::vector<std::string> expectedHistogram{
            "BJN|0|7000|4",    "BJN|1|120000|4", "BJN|1|60000|2",   "CYGY|0|7000|4",
            "CYGY|1|120000|6", "CYGY|1|60000|2", "SJS|0|7000|4",    "SJS|1|120000|2",
            "SJS|1|60000|2",   "TZYH|0|7000|4",  "TZYH|1|120000|2", "TZYH|1|60000|2",
            "ZGC|0|7000|4",    "ZGC|1|120000|4", "ZGC|1|60000|2"};
        std::sort(histogram.begin(), histogram.end());
        tests.check(histogram == expectedHistogram,
                    "per-station device counts match the SRS power tables");
        const std::vector<std::string> faulty =
            reader.rows("SELECT code FROM charger WHERE status=2 ORDER BY code");
        tests.check(faulty == std::vector<std::string>{"BJN-DC-01", "CYGY-AC-01", "CYGY-DC-01",
                                                       "SJS-AC-01", "TZYH-DC-01", "ZGC-DC-01"},
                    "the six fixed faulty devices carry status 2");
        tests.check(reader.integer("SELECT COUNT(*) FROM charger WHERE (charger_type=1 AND "
                                   "connector_standard<>'GB/T 20234.3') OR (charger_type=0 AND "
                                   "connector_standard<>'GB/T 20234.2')") == 0,
                    "DC chargers use GB/T 20234.3 and AC chargers GB/T 20234.2");

        std::vector<std::string> tariffs = reader.rows(
            "SELECT adcode||'|'||electricity_cent_per_kwh||'|'||service_cent_per_kwh FROM "
            "region_tariff WHERE effective_from=0 ORDER BY adcode");
        tests.check(tariffs == std::vector<std::string>{"110105|90|55", "110106|80|45",
                                                        "110107|75|40", "110108|85|50",
                                                        "110112|70|35"} &&
                        reader.integer("SELECT COUNT(*) FROM region_tariff") == 5,
                    "five-zone fixed tariffs are seeded per SRS");

        tests.check(reader.integer("SELECT COUNT(*) FROM user_account WHERE username LIKE "
                                   "'sim_owner_%'") == 300 &&
                        reader.integer("SELECT COUNT(*) FROM user_account") == 300 &&
                        reader.integer("SELECT COUNT(*) FROM user_account WHERE phone LIKE "
                                       "'%9999'") == 0 &&
                        reader.integer("SELECT COUNT(*) FROM user_account WHERE status<>1 OR "
                                       "has_active_flow<>0") == 0 &&
                        reader.integer("SELECT COUNT(*) FROM wallet_account") == 300,
                    "300 simulated owners are registered with idle status and wallets");

        const std::int64_t orders = reader.integer("SELECT COUNT(*) FROM charging_order");
        checkIntegerIn(tests, reader, "SELECT COUNT(*) FROM charging_order", 8600, 9400,
                       "about 9000 historical orders are seeded");
        const std::vector<std::string> daily = reader.rows(
            "SELECT printf('%d|%d',created_at/86400,COUNT(*)) FROM charging_order GROUP BY "
            "created_at/86400");
        bool dailyOk = daily.size() == 90;
        std::int64_t firstDay = 0;
        for (std::size_t index = 0; dailyOk && index < daily.size(); ++index)
        {
            const auto parts = integers(daily[index]);
            if (index == 0)
                firstDay = parts[0];
            dailyOk = parts[0] == firstDay + static_cast<std::int64_t>(index) && parts[1] >= 80 &&
                      parts[1] <= 120;
        }
        tests.check(dailyOk && firstDay == windowBegin / 86400,
                    "the 90-day window has 80-120 orders per day with no gaps");

        const std::int64_t completed =
            reader.integer("SELECT COUNT(*) FROM charging_order WHERE status=60");
        const std::int64_t cancelled =
            reader.integer("SELECT COUNT(*) FROM charging_order WHERE status=70");
        tests.check(completed + cancelled == orders &&
                        reader.integer("SELECT COUNT(*) FROM charging_order WHERE status NOT IN "
                                       "(60,70)") == 0 &&
                        completed * 1000 / orders >= 925 && completed * 1000 / orders <= 955,
                    "roughly 94% completed and 6% cancelled with no settlement failures");
        const std::int64_t fast =
            reader.integer("SELECT COUNT(*) FROM charging_order WHERE charger_type=1");
        tests.check(fast * 1000 / orders >= 660 && fast * 1000 / orders <= 740,
                    "about 70% fast DC and 30% slow AC orders");

        // Station ids follow the SRS order ZGC..TZYH on every seeded database.
        const std::vector<std::string> shares = reader.rows(
            "SELECT COUNT(*) FROM charging_order GROUP BY station_id ORDER BY station_id");
        const std::int64_t expectedShares[] = {24, 28, 22, 14, 12};
        bool shareOk = shares.size() == 5;
        for (std::size_t index = 0; shareOk && index < shares.size(); ++index)
        {
            const auto parts = integers(shares[index]);
            shareOk = parts[0] * 1000 / orders >= (expectedShares[index] - 3) * 10 &&
                      parts[0] * 1000 / orders <= (expectedShares[index] + 3) * 10;
        }
        tests.check(shareOk, "five-station order shares follow 24/28/22/14/12 within tolerance");

        std::int64_t weekdayTotal = 0;
        std::int64_t weekdayPeak = 0;
        std::int64_t weekendTotal = 0;
        std::int64_t weekendPeak = 0;
        for (const auto& cell : reader.rows(
                 "SELECT printf('%d|%d|%d',strftime('%w',started_at,'unixepoch') IN ('0','6'),"
                 "CAST(strftime('%H',started_at,'unixepoch') AS INTEGER) IN (7,8,17,18,19,20),"
                 "CAST(strftime('%H',started_at,'unixepoch') AS INTEGER) BETWEEN 10 AND 19) "
                 "FROM charging_order WHERE status=60"))
        {
            const auto parts = integers(cell);
            if (parts[0] == 0)
            {
                ++weekdayTotal;
                weekdayPeak += parts[1];
            }
            else
            {
                ++weekendTotal;
                weekendPeak += parts[2];
            }
        }
        tests.check(weekdayTotal > 0 && weekdayPeak * 1000 / weekdayTotal >= 520,
                    "weekday orders peak inside 7:00-9:00 and 17:00-21:00");
        tests.check(weekendTotal > 0 && weekendPeak * 1000 / weekendTotal >= 640,
                    "weekend orders peak inside 10:00-20:00");

        tests.check(reader.integer("SELECT COUNT(*) FROM charging_flow") == orders &&
                        reader.integer("SELECT COUNT(*) FROM charging_flow WHERE status NOT IN "
                                       "(60,70)") == 0 &&
                        reader.integer("SELECT COUNT(*) FROM charging_order o LEFT JOIN "
                                       "charging_flow f ON f.flow_no=o.flow_no WHERE "
                                       "f.flow_no IS NULL") == 0,
                    "every historical order has one terminal flow");
        tests.check(reader.integer("SELECT COUNT(*) FROM charging_order o JOIN charger c ON "
                                   "c.id=o.charger_id WHERE o.charger_code<>c.code OR "
                                   "o.charger_type<>c.charger_type OR o.power_watt<>c.power_watt "
                                   "OR o.station_id<>c.station_id") == 0 &&
                        reader.integer("SELECT COUNT(*) FROM charging_order o JOIN charger c ON "
                                       "c.id=o.charger_id WHERE c.status=2") == 0,
                    "order snapshots match their chargers and never use faulty devices");
        tests.check(reader.integer("SELECT COUNT(*) FROM charging_order WHERE status=60 AND "
                                   "(started_at IS NULL OR ended_at IS NULL OR settled_at IS "
                                   "NULL OR settled_at<>ended_at)") == 0 &&
                        reader.integer("SELECT COUNT(*) FROM charging_order WHERE status=70 AND "
                                       "settled_at IS NOT NULL") == 0,
                    "completed orders carry a full settled timeline");
        tests.check(reader.integer("SELECT COUNT(*) FROM charging_flow f WHERE (f.status=60 AND "
                                   "(SELECT COUNT(*) FROM flow_event WHERE "
                                   "flow_no=f.flow_no)<>4) OR (f.status=70 AND (SELECT COUNT(*) "
                                   "FROM flow_event WHERE flow_no=f.flow_no)<>3)") == 0 &&
                        reader.rows("SELECT printf('%d|%d|%d',f.status,fe.to_status,COUNT(*)) "
                                    "FROM charging_flow f JOIN flow_event fe ON "
                                    "fe.flow_no=f.flow_no GROUP BY f.status,fe.to_status")
                                .size() == 7,
                    "completed flows log four events and cancelled flows three");
        tests.check(reader.integer("SELECT COUNT(*) FROM charging_order WHERE status=60 AND "
                                   "energy_mwh<>(power_watt*(ended_at-started_at)*60*10)/36") ==
                            0 &&
                        reader.integer("SELECT COUNT(*) FROM charging_order WHERE status=60 AND "
                                       "amount_cent<>(energy_mwh*(electricity_price+"
                                       "service_price)+500000)/1000000") == 0 &&
                        reader.integer("SELECT COUNT(*) FROM charging_order WHERE status=60 AND "
                                       "(paid_cent<>amount_cent OR debt_added_cent<>0 OR "
                                       "debt_after_cent<>0)") == 0,
                    "energy, amount and payment math match the pricing formula");
        tests.check(reader.integer("SELECT COUNT(*) FROM charging_order WHERE time_scale<>60 OR "
                                   "station_name<>(SELECT name FROM station WHERE "
                                   "id=charging_order.station_id)") == 0,
                    "orders carry the 60x time scale and a station name snapshot");

        checkIntegerIn(tests, reader, "SELECT COUNT(*) FROM recharge_order", 820, 1100,
                       "about 900 recharge orders are seeded");
        tests.check(reader.integer("SELECT MIN(requested_cent) FROM recharge_order") >= 500 &&
                        reader.integer("SELECT COUNT(*) FROM recharge_order WHERE "
                                       "requested_cent<>balance_added_cent OR "
                                       "debt_paid_cent<>0") == 0 &&
                        reader.integer("SELECT COUNT(*) FROM wallet_transaction WHERE type=0") ==
                            reader.integer("SELECT COUNT(*) FROM recharge_order") &&
                        reader.integer("SELECT COUNT(*) FROM wallet_transaction WHERE type=1") ==
                            completed,
                    "recharges respect the 500-cent minimum and pair with wallet transactions");

        bool ledgerOk = true;
        bool mirrorOk = true;
        for (const auto& userId : reader.rows("SELECT id FROM user_account WHERE username LIKE "
                                              "'sim_owner_%'"))
        {
            const std::string scope = " WHERE user_id=" + userId;
            std::int64_t running = 0;
            bool solvency = true;
            for (const auto& txn : reader.rows("SELECT printf('%d|%d|%d|%d',type,amount_cent,"
                                               "balance_after_cent,debt_after_cent) FROM "
                                               "wallet_transaction" +
                                               scope + " ORDER BY id"))
            {
                const auto parts = integers(txn);
                running += parts[1];
                solvency = solvency && running >= 0 && parts[2] == running && parts[3] == 0 &&
                           ((parts[0] == 0 && parts[1] > 0) || (parts[0] == 1 && parts[1] < 0));
            }
            const auto mirrors = integers(
                reader
                    .rows("SELECT printf('%d|%d|%d|%d',(SELECT balance_cent FROM wallet_account" +
                          scope + "),(SELECT debt_cent FROM wallet_account" + scope +
                          "),(SELECT balance_cent FROM user_account WHERE id=" + userId +
                          "),(SELECT debt_cent FROM user_account WHERE id=" + userId + "))")
                    .front());
            ledgerOk = ledgerOk && solvency;
            mirrorOk = mirrorOk && mirrors[0] == mirrors[2] && mirrors[1] == mirrors[3] &&
                       mirrors[0] == running;
        }
        tests.check(ledgerOk, "every owner ledger replays without debt or negative balances");
        tests.check(mirrorOk, "wallet rows mirror user balances after the full history");

        tests.check(
            reader.integer("SELECT COUNT(*) FROM charger c LEFT JOIN (SELECT charger_id,COUNT(*) "
                           "AS cnt,COALESCE(SUM(ended_at-started_at),0) AS minutes FROM "
                           "charging_order WHERE status=60 GROUP BY charger_id) o ON "
                           "o.charger_id=c.id WHERE c.total_count<>COALESCE(o.cnt,0) OR "
                           "c.total_minutes<>COALESCE(o.minutes,0)") == 0,
            "charger cumulative counters equal the completed order aggregates");
    }

    {
        // Reopening a seeded database must not duplicate any row.
        TemporaryDatabase database;
        const std::string& db = database.path();
        const auto seedCounts = [&]
        {
            DatabaseReader reader(db);
            return reader
                .rows("SELECT (SELECT COUNT(*) FROM station)||'|'||(SELECT COUNT(*) "
                      "FROM charger)||'|'||(SELECT COUNT(*) FROM user_account WHERE "
                      "username LIKE 'sim_owner_%')||'|'||(SELECT COUNT(*) FROM "
                      "charging_order)||'|'||(SELECT COUNT(*) FROM charging_flow)||'|'||"
                      "(SELECT COUNT(*) FROM recharge_order)||'|'||(SELECT COUNT(*) "
                      "FROM schema_version)")
                .front();
        };
        {
            SqliteRepository repository(db);
        }
        const std::string before = seedCounts();
        tests.check(!before.empty() && before.find('|') != std::string::npos,
                    "first open seeds the full demo dataset");
        {
            SqliteRepository repository(db);
        }
        tests.check(seedCounts() == before &&
                        DatabaseReader(db).integer("SELECT COUNT(*) FROM schema_version WHERE "
                                                   "version=8") == 1,
                    "reopening the database changes no row counts and keeps one v8 row");
    }

    {
        // An existing v1-v7 database upgrades through v8 once: business data
        // survives, unreferenced XEQ/CBD leftovers are removed and later opens
        // stay idempotent.
        TemporaryDatabase upgradeDatabase;
        const std::string& db = upgradeDatabase.path();
        std::int64_t upgradeUserId = 0;
        std::int64_t upgradeBalance = 0;
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        {
            SqliteRepository repository(db);
            repository.ensureDevelopmentAdmin(true);
            UserAccount account;
            account.username = "upgrade_owner";
            account.phone = "13800137777";
            account.nickname = "升级用户";
            account.registeredAt = now - 3600;
            tests.check(repository.create(account) == AccountWriteResult::Success,
                        "upgrade database seeds a business owner");
            upgradeUserId = account.id;
            BusinessNumbers numbers(&repository);
            WalletService wallet(repository, repository, numbers);
            tests.check(wallet.recharge(upgradeUserId, 7000, std::chrono::system_clock::now()).ok(),
                        "upgrade database funds the business owner");
            upgradeBalance = repository.wallet(upgradeUserId).balanceCent;
        }
        unseed(db);
        {
            SqliteRepository repository(db);
            tests.check(repository.findById(upgradeUserId).has_value() &&
                            repository.wallet(upgradeUserId).balanceCent == upgradeBalance &&
                            repository.findAdminByUsername("admin").has_value() &&
                            DatabaseReader(db).integer("SELECT COUNT(*) FROM station") == 5 &&
                            DatabaseReader(db).integer("SELECT COUNT(*) FROM user_account WHERE "
                                                       "username LIKE 'sim_owner_%'") == 300 &&
                            DatabaseReader(db).integer("SELECT COUNT(*) FROM charger") == 48,
                        "upgrading a v1-v7 database re-runs v8, keeps business data and "
                        "removes the unreferenced XEQ/CBD leftovers");
            DatabaseReader reader(db);
            tests.check(reader.integer("SELECT COUNT(*) FROM schema_version") == 8 &&
                            reader.integer("SELECT COUNT(*) FROM charging_flow") ==
                                reader.integer("SELECT COUNT(*) FROM charging_order") &&
                            reader.integer("SELECT COUNT(*) FROM charging_order") >= 8600,
                        "the upgraded seed is consistent and free of duplicates");
        }
        DatabaseReader reader(db);
        tests.check(reader.integer("SELECT COUNT(*) FROM user_account WHERE username LIKE "
                                   "'sim_owner_%'") == 300,
                    "another open of the upgraded database still does not duplicate");
    }

    {
        // Retention deviation: a pre-v8 flow referencing XEQ keeps the station
        // and its chargers, while unreferenced CBD is still removed.
        TemporaryDatabase database;
        const std::string& db = database.path();
        std::int64_t ownerId = 0;
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        {
            SqliteRepository repository(db);
            UserAccount account;
            account.username = "upgrade_owner2";
            account.phone = "13800136666";
            account.nickname = "保留用户";
            account.registeredAt = now - 100 * 86400;
            tests.check(repository.create(account) == AccountWriteResult::Success,
                        "retention database seeds a legacy owner");
            ownerId = account.id;
        }
        unseed(db);
        const std::int64_t legacyAt = now - 95 * 86400;
        executeSql(db, "INSERT INTO charging_flow(flow_no,user_id,station_id,charger_type,"
                       "charger_id,charger_code,status,version,created_at) VALUES "
                       "('FLXEQLEGACY0001'," +
                           std::to_string(ownerId) + ",2,1,6,'XEQ-DC-01',60,4," +
                           std::to_string(legacyAt) + ")");
        executeSql(db, "INSERT INTO charging_order(order_no,flow_no,user_id,station_id,"
                       "station_name,charger_id,charger_code,charger_type,electricity_price,"
                       "service_price,power_watt,time_scale,status,created_at,started_at,"
                       "ended_at,settled_at) VALUES "
                       "('ORXEQLEGACY0001','FLXEQLEGACY0001'," +
                           std::to_string(ownerId) +
                           ",2,'NCS 西二旗充电站',6,'XEQ-DC-01',1,85,50,120000,60,60," +
                           std::to_string(legacyAt) + "," + std::to_string(legacyAt + 30) + "," +
                           std::to_string(legacyAt + 50) + "," + std::to_string(legacyAt + 50) +
                           ")");
        {
            SqliteRepository repository(db);
            DatabaseReader reader(db);
            tests.check(
                reader.rows("SELECT code FROM station ORDER BY code") ==
                        std::vector<std::string>{"BJN", "CYGY", "SJS", "TZYH", "XEQ", "ZGC"} &&
                    reader.integer("SELECT COUNT(*) FROM charger WHERE station_id=(SELECT "
                                   "id FROM station WHERE code='XEQ')") == 2 &&
                    reader.integer("SELECT COUNT(*) FROM station WHERE code='CBD'") == 0 &&
                    reader.integer("SELECT COUNT(*) FROM charging_flow WHERE "
                                   "flow_no='FLXEQLEGACY0001'") == 1 &&
                    reader.integer("SELECT COUNT(*) FROM charging_order WHERE "
                                   "order_no='ORXEQLEGACY0001'") == 1 &&
                    reader.integer("SELECT COUNT(*) FROM user_account WHERE username LIKE "
                                   "'sim_owner_%'") == 300,
                "referenced XEQ survives the seed while unreferenced CBD is removed");
        }
    }

    {
        // refreshHourlyMetrics over the seeded window rebuilds a gap-free
        // hourly series, including zero-fill for quiet hours.
        TemporaryDatabase database;
        const std::string& db = database.path();
        std::int64_t windowBegin = 0;
        std::int64_t anchorDay = 0;
        std::int64_t completed = 0;
        {
            SqliteRepository repository(db);
            DatabaseReader reader(db);
            anchorDay = reader.integer("SELECT (applied_at/86400)*86400 FROM schema_version "
                                       "WHERE version=8");
            windowBegin = anchorDay - 90 * 86400;
            completed = reader.integer("SELECT COUNT(*) FROM charging_order WHERE status=60");
            repository.refreshHourlyMetrics(windowBegin, anchorDay);
        }
        DatabaseReader reader(db);
        tests.check(reader.integer("SELECT COUNT(*) FROM station_hourly_metric") == 90 * 24 * 5 &&
                        reader.integer("SELECT COUNT(*) FROM (SELECT station_id FROM "
                                       "station_hourly_metric GROUP BY station_id HAVING "
                                       "COUNT(*)<90*24)") == 0 &&
                        reader.integer("SELECT MIN(bucket_at) FROM station_hourly_metric") ==
                            windowBegin &&
                        reader.integer("SELECT MAX(bucket_at) FROM station_hourly_metric") ==
                            anchorDay - 3600,
                    "hourly metrics cover every station with 90 complete days, no gaps");
        tests.check(reader.integer("SELECT SUM(order_count) FROM station_hourly_metric") ==
                            completed &&
                        reader.integer("SELECT SUM(energy_mwh) FROM station_hourly_metric") ==
                            reader.integer("SELECT SUM(energy_mwh) FROM charging_order WHERE "
                                           "status=60") &&
                        reader.integer("SELECT COUNT(*) FROM station_hourly_metric WHERE "
                                       "order_count=0") > 0,
                    "rebuilt series aggregates all completed orders and zero-fills quiet hours");
        executeSql(db, "DELETE FROM station_hourly_metric");
        {
            SqliteRepository repository(db);
            repository.refreshHourlyMetrics(windowBegin, anchorDay);
        }
        tests.check(reader.integer("SELECT COUNT(*) FROM station_hourly_metric") == 90 * 24 * 5,
                    "re-running the rebuild reproduces the same gap-free series");
    }

    return tests.result();
}
