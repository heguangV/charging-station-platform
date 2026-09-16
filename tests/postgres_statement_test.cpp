#include "infrastructure/postgres/postgres_repository_detail.h"

#include <QCoreApplication>

#include <iostream>
#include <stdexcept>

using namespace ncs::infrastructure::postgres::detail;

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try
    {
        for (const auto* mode : {"on", "local", "remote_write", "remote_apply"})
            if (!durableWalSettings("on", "on", mode))
                throw std::runtime_error("durable configuration rejected");
        if (durableWalSettings("off", "on", "on") || durableWalSettings("on", "off", "on") ||
            durableWalSettings("on", "on", "off") || durableWalSettings("on", "on", "unknown"))
            throw std::runtime_error("unsafe configuration accepted");

        // Driver-independent wrapper regression, not a PostgreSQL integration test.
        auto database = QSqlDatabase::addDatabase("QSQLITE", "statement-unit");
        database.setDatabaseName(":memory:");
        if (!database.open())
            throw std::runtime_error("unit database unavailable");
        Statement literal(&database, "SELECT ':p1', ?, '12:30'");
        literal.bind(1, 42);
        if (!literal.row() || literal.text(0) != ":p1" || literal.integer(1) != 42 ||
            literal.text(2) != "12:30")
            throw std::runtime_error("literal changed placeholder binding");
        execute(&database, "CREATE TABLE secret_values(value TEXT UNIQUE)");
        execute(&database, "INSERT INTO secret_values VALUES('private-business-value')");
        for (const auto* sql : {"SELECT * FROM secret_missing_table",
                                "INSERT INTO secret_values VALUES('private-business-value')"})
        {
            bool failed = false;
            try
            {
                Statement statement(&database, sql);
                statement.execute();
            }
            catch (const std::runtime_error& error)
            {
                failed = true;
                if (std::string(error.what()) != "database statement failed")
                    throw std::runtime_error("database error was not redacted");
            }
            if (!failed)
                throw std::runtime_error("invalid SQL unexpectedly succeeded");
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
