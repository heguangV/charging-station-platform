#include "infrastructure/sqlite/sqlite_repository.h"

#include <QCoreApplication>

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include <sqlite3.h>

namespace
{

void execute(const char* path, const char* sql)
{
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(path, &database, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK)
        throw std::runtime_error("fixture database open failed");
    char* error = nullptr;
    const int result = sqlite3_exec(database, sql, nullptr, nullptr, &error);
    const std::string message = error == nullptr ? "fixture SQL failed" : error;
    sqlite3_free(error);
    sqlite3_close(database);
    if (result != SQLITE_OK)
        throw std::runtime_error(message);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc < 2 || argc > 3)
    {
        std::cerr << "usage: ncs_create_sqlite_v10_fixture <path>\n";
        return 2;
    }
    try
    {
        {
            ncs::infrastructure::sqlite::SqliteRepository repository(argv[1]);
            const auto readiness = repository.check();
            if (!readiness.ready())
            {
                std::cerr << "fixture readiness check failed\n";
                return 3;
            }
        }
        const std::string_view mode = argc == 3 ? argv[2] : "";
        if (mode == "--add-user")
            execute(argv[1],
                    "INSERT INTO user_account(id,username,phone,nickname,status,registered_at,"
                    "balance_cent,debt_cent,has_active_flow,version,deleted) VALUES(301,"
                    "'migration_user','13900000001','migration-user',1,1789401600,0,0,0,1,0);"
                    "INSERT INTO wallet_account(user_id,balance_cent,debt_cent,version,updated_at)"
                    " VALUES(301,0,0,1,1789401600);");
        else if (mode == "--pg-constraint-violation")
            execute(argv[1], "PRAGMA ignore_check_constraints=ON;"
                             "UPDATE user_account SET status=99 WHERE id=1;");
        else if (!mode.empty())
            throw std::invalid_argument("unknown fixture mode");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 4;
    }
}
