#include "infrastructure/sqlite/sqlite_repository.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

#include <sqlite3.h>

namespace
{

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

std::string uniqueDatabaseName()
{
    const auto random = std::random_device{}();
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    return "ncs-seed-conflict-" + std::to_string(random) + "-" + std::to_string(nanos) + ".db";
}

class TemporaryDatabase final
{
  public:
    TemporaryDatabase() : path_(std::filesystem::temp_directory_path() / uniqueDatabaseName()) {}

    ~TemporaryDatabase()
    {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(path_.string() + "-wal", ignored);
        std::filesystem::remove(path_.string() + "-shm", ignored);
    }

    std::string path() const
    {
        return path_.string();
    }

  private:
    std::filesystem::path path_;
};

void executeSql(const std::string& path, const std::string& sql)
{
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK)
        throw std::runtime_error("test database open failed");
    char* error = nullptr;
    const int result = sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &error);
    const std::string message = error ? error : "test SQL failed";
    sqlite3_free(error);
    sqlite3_close(database);
    if (result != SQLITE_OK)
        throw std::runtime_error(message);
}

std::int64_t queryInteger(const std::string& path, const std::string& sql)
{
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        throw std::runtime_error("test database open failed");
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
    {
        const std::string message = sqlite3_errmsg(database);
        sqlite3_close(database);
        throw std::runtime_error(message);
    }
    const std::int64_t value =
        sqlite3_step(statement) == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : -1;
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return value;
}

std::string queryText(const std::string& path, const std::string& sql)
{
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        throw std::runtime_error("test database open failed");
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
    {
        const std::string message = sqlite3_errmsg(database);
        sqlite3_close(database);
        throw std::runtime_error(message);
    }
    std::string value;
    if (sqlite3_step(statement) == SQLITE_ROW)
    {
        const auto* text = sqlite3_column_text(statement, 0);
        if (text)
            value = reinterpret_cast<const char*>(text);
    }
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return value;
}

void resetToPreV8(const std::string& path)
{
    {
        ncs::infrastructure::sqlite::SqliteRepository repository(path);
    }
    executeSql(path,
               "DELETE FROM flow_event WHERE flow_no IN (SELECT flow_no FROM charging_flow WHERE "
               "user_id IN (SELECT id FROM user_account WHERE username GLOB "
               "'sim_owner_[0-9][0-9][0-9]'));"
               "DELETE FROM charging_order WHERE user_id IN (SELECT id FROM user_account WHERE "
               "username GLOB 'sim_owner_[0-9][0-9][0-9]');"
               "DELETE FROM charging_flow WHERE user_id IN (SELECT id FROM user_account WHERE "
               "username GLOB 'sim_owner_[0-9][0-9][0-9]');"
               "DELETE FROM wallet_transaction WHERE user_id IN (SELECT id FROM user_account "
               "WHERE username GLOB 'sim_owner_[0-9][0-9][0-9]');"
               "DELETE FROM recharge_order WHERE user_id IN (SELECT id FROM user_account WHERE "
               "username GLOB 'sim_owner_[0-9][0-9][0-9]');"
               "DELETE FROM wallet_account WHERE user_id IN (SELECT id FROM user_account WHERE "
               "username GLOB 'sim_owner_[0-9][0-9][0-9]');"
               "DELETE FROM user_account WHERE username GLOB 'sim_owner_[0-9][0-9][0-9]';"
               "DELETE FROM charger WHERE station_id IN (SELECT id FROM station WHERE code IN "
               "('CYGY','BJN','SJS','TZYH')) OR code IN ('ZGC-DC-04','ZGC-DC-05','ZGC-DC-06',"
               "'ZGC-AC-03','ZGC-AC-04');"
               "DELETE FROM station WHERE code IN ('CYGY','BJN','SJS','TZYH');"
               "DELETE FROM region_tariff WHERE adcode IN ('110106','110107','110112');"
               "DELETE FROM schema_version WHERE version=8;");
}

bool openThrows(const std::string& path, std::string& message)
{
    try
    {
        ncs::infrastructure::sqlite::SqliteRepository repository(path);
        return false;
    }
    catch (const std::runtime_error& error)
    {
        message = error.what();
        return true;
    }
}

void insertAccount(const std::string& path, const std::string_view username,
                   const std::string_view phone)
{
    executeSql(path,
               "INSERT INTO user_account(username,phone,nickname,status,registered_at,balance_cent,"
               "debt_cent,has_active_flow,version,deleted) VALUES ('" +
                   std::string(username) + "','" + std::string(phone) +
                   "','测试车主',1,1788500000,5000,0,0,1,0)");
}

} // namespace

int main()
{
    TestRunner tests;

    {
        TemporaryDatabase database;
        resetToPreV8(database.path());
        insertAccount(database.path(), "sim_owner_001", "13700000001");
        executeSql(database.path(),
                   "INSERT INTO wallet_account(user_id,balance_cent,debt_cent,version,updated_at) "
                   "SELECT id,5000,0,1,registered_at FROM user_account WHERE "
                   "username='sim_owner_001'");
        std::string message;
        tests.check(openThrows(database.path(), message) &&
                        message.find("sim_owner_001") != std::string::npos,
                    "an exact username collision aborts with a useful error");
        tests.check(queryInteger(database.path(), "SELECT MAX(version) FROM schema_version") == 7 &&
                        queryInteger(database.path(), "SELECT COUNT(*) FROM wallet_account") == 1,
                    "the username collision rolls back v8 and preserves the account wallet");
    }

    {
        TemporaryDatabase database;
        resetToPreV8(database.path());
        insertAccount(database.path(), "real_owner_001", "13800001001");
        std::string message;
        tests.check(openThrows(database.path(), message) &&
                        message.find("phone identity") != std::string::npos &&
                        message.find("13800001001") == std::string::npos,
                    "a phone collision aborts without exposing the complete phone");
        tests.check(queryInteger(database.path(), "SELECT MAX(version) FROM schema_version") == 7 &&
                        queryInteger(database.path(), "SELECT COUNT(*) FROM user_account WHERE "
                                                      "username='real_owner_001'") == 1,
                    "the phone collision rolls back v8 and preserves the account");
    }

    {
        TemporaryDatabase database;
        resetToPreV8(database.path());
        insertAccount(database.path(), "simXowner_001", "13700000002");
        executeSql(database.path(),
                   "INSERT INTO region_tariff(adcode,electricity_cent_per_kwh,"
                   "service_cent_per_kwh,effective_from,effective_to) VALUES "
                   "('110106',99,66,1234567890,1788500000);"
                   "INSERT INTO station(id,code,name,address,adcode,latitude_e6,longitude_e6,"
                   "business_hours,enabled) VALUES (20,'BJN','历史北京南站','自定义历史地址',"
                   "'110106',39900000,116400000,'00:00-24:00',1)");
        {
            ncs::infrastructure::sqlite::SqliteRepository repository(database.path());
        }
        tests.check(queryInteger(database.path(), "SELECT COUNT(*) FROM user_account WHERE "
                                                  "username='simXowner_001'") == 1 &&
                        queryInteger(database.path(), "SELECT COUNT(*) FROM user_account WHERE "
                                                      "username GLOB "
                                                      "'sim_owner_[0-9][0-9][0-9]'") == 300,
                    "a look-alike username survives the exact conflict check");
        tests.check(queryText(database.path(), "SELECT name FROM station WHERE code='BJN'") ==
                            "历史北京南站" &&
                        queryInteger(database.path(), "SELECT COUNT(*) FROM region_tariff WHERE "
                                                      "adcode='110106' AND "
                                                      "electricity_cent_per_kwh=99") == 1,
                    "an existing station and historical tariff survive the seed");
    }

    return tests.result();
}
