// One-shot SQLite v10 to PostgreSQL v10 data migration utility. The target must be a
// empty PostgreSQL database. Schema creation and import commit as one transaction.
#include "infrastructure/postgres/postgres_config.h"
#include "infrastructure/postgres/postgres_migrations.h"

#include <QCoreApplication>
#include <QProcessEnvironment>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>

#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

using ncs::infrastructure::postgres::PostgresConfig;

constexpr std::array<const char*, 28> kTables{{
    "station",
    "user_account",
    "user_credential",
    "user_avatar",
    "wallet_account",
    "region_tariff",
    "charger",
    "admin_account",
    "admin_role",
    "business_sequence",
    "idempotency_record",
    "recharge_order",
    "wallet_transaction",
    "charging_flow",
    "flow_queue",
    "charging_order",
    "flow_event",
    "outbox_event",
    "ops_log",
    "price_adjustment",
    "device_command",
    "ml_task",
    "backup_record",
    "station_hourly_metric",
    "model_version",
    "load_prediction",
    "dashboard_state",
    "order_review",
}};

constexpr std::array<const char*, 11> kIdentityTables{{
    "user_account",
    "station",
    "charger",
    "region_tariff",
    "wallet_transaction",
    "flow_event",
    "outbox_event",
    "admin_account",
    "ops_log",
    "price_adjustment",
    "flow_queue",
}};

std::string errorText(const QSqlQuery& query)
{
    (void)query;
    // PostgreSQL DETAIL can contain full business rows; do not print it.
    return "database statement failed";
}

void execute(QSqlDatabase& database, const QString& sql)
{
    QSqlQuery query(database);
    if (!query.exec(sql))
        throw std::runtime_error(errorText(query));
}

std::int64_t scalar(QSqlDatabase& database, const QString& sql)
{
    QSqlQuery query(database);
    if (!query.exec(sql) || !query.next())
        throw std::runtime_error(errorText(query));
    return query.value(0).toLongLong();
}

class Database final
{
  public:
    Database(QString name, QString driver) : name_(std::move(name))
    {
        database_ = QSqlDatabase::addDatabase(driver, name_);
    }

    ~Database()
    {
        database_.close();
        database_ = {};
        QSqlDatabase::removeDatabase(name_);
    }

    QSqlDatabase& get()
    {
        return database_;
    }

  private:
    QString name_;
    QSqlDatabase database_;
};

struct Options
{
    std::string sqlitePath;
    PostgresConfig postgres;
    bool confirmed = false;
};

std::string env(const char* key, const char* fallback = "")
{
    const QByteArray value = qgetenv(key);
    return value.isEmpty() ? fallback : value.toStdString();
}

int integerEnv(const char* key, const int fallback)
{
    const std::string value = env(key);
    return value.empty() ? fallback : std::stoi(value);
}

Options parse(int argc, char** argv)
{
    Options options;
    options.postgres.host = env("NCS_DATABASE_HOST", "127.0.0.1");
    options.postgres.port = static_cast<std::uint16_t>(integerEnv("NCS_DATABASE_PORT", 5432));
    options.postgres.database = env("NCS_DATABASE_NAME", "ncs");
    options.postgres.user = env("NCS_DATABASE_USER", "ncs");
    options.postgres.password = env("NCS_DATABASE_PASSWORD");
    options.postgres.sslMode = env("NCS_DATABASE_SSLMODE", "prefer");
    options.postgres.sslRootCertificate = env("NCS_DATABASE_SSL_ROOT_CERT");
    options.postgres.migrationsDirectory =
        env("NCS_DATABASE_MIGRATIONS", NCS_POSTGRES_MIGRATIONS_DIRECTORY);

    const std::map<std::string, std::string*> strings{{
        {"--sqlite", &options.sqlitePath},
        {"--database-host", &options.postgres.host},
        {"--database-name", &options.postgres.database},
        {"--database-user", &options.postgres.user},
        {"--database-sslmode", &options.postgres.sslMode},
        {"--database-ssl-root-cert", &options.postgres.sslRootCertificate},
        {"--database-migrations", &options.postgres.migrationsDirectory},
    }};
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument == "--confirm-fresh-target")
        {
            options.confirmed = true;
            continue;
        }
        if (argument == "--database-port")
        {
            if (++i >= argc)
                throw std::invalid_argument("missing value for --database-port");
            const int port = std::stoi(argv[i]);
            if (port < 1 || port > 65535)
                throw std::invalid_argument("database port must be between 1 and 65535");
            options.postgres.port = static_cast<std::uint16_t>(port);
            continue;
        }
        const auto found = strings.find(argument);
        if (found == strings.end() || ++i >= argc)
            throw std::invalid_argument("unknown option or missing value: " + argument);
        *found->second = argv[i];
    }
    if (options.sqlitePath.empty() || !options.confirmed)
        throw std::invalid_argument(
            "usage: ncs_sqlite_to_postgres --sqlite <v10.db> --confirm-fresh-target "
            "[PostgreSQL options]; password is read only from NCS_DATABASE_PASSWORD");
    if (!std::filesystem::is_regular_file(options.sqlitePath))
        throw std::invalid_argument("SQLite source is not a regular file");
    return options;
}

void openSource(QSqlDatabase& database, const Options& options)
{
    database.setDatabaseName(QString::fromStdString(options.sqlitePath));
    database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    if (!database.open())
        throw std::runtime_error("SQLite source cannot be opened read-only");
    // Pin one source snapshot for metadata, integrity checks and every copied table.
    execute(database, QStringLiteral("BEGIN"));
    QSqlQuery version(database);
    if (!version.exec(QStringLiteral(
            "SELECT version,checksum FROM schema_version ORDER BY version DESC LIMIT 1")) ||
        !version.next() || version.value(0).toInt() != 10 ||
        version.value(1).toString() != QStringLiteral("ncs-v10-order-confirmation"))
        throw std::runtime_error("SQLite source must be a verified NCS v10 database");
    QSqlQuery foreignKeys(database);
    if (!foreignKeys.exec(QStringLiteral("PRAGMA foreign_key_check")))
        throw std::runtime_error(errorText(foreignKeys));
    if (foreignKeys.next())
        throw std::runtime_error("SQLite source failed its foreign-key integrity check");
}

void openTarget(QSqlDatabase& database, const Options& options)
{
    const auto& config = options.postgres;
    database.setHostName(QString::fromStdString(config.host));
    database.setPort(config.port);
    database.setDatabaseName(QString::fromStdString(config.database));
    database.setUserName(QString::fromStdString(config.user));
    database.setPassword(QString::fromStdString(config.password));
    QString connectOptions =
        QStringLiteral("sslmode=%1").arg(QString::fromStdString(config.sslMode));
    if (!config.sslRootCertificate.empty())
        connectOptions += QStringLiteral(";sslrootcert=%1")
                              .arg(QString::fromStdString(config.sslRootCertificate));
    database.setConnectOptions(connectOptions);
    if (!database.open())
        throw std::runtime_error("PostgreSQL target connection failed");
}

void verifyFreshTarget(QSqlDatabase& database)
{
    if (scalar(database,
               QStringLiteral(
                   "SELECT COUNT(*) FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace "
                   "WHERE n.nspname NOT IN ('pg_catalog','information_schema') "
                   "AND n.nspname NOT LIKE 'pg_toast%' AND n.nspname NOT LIKE 'pg_temp_%'")) != 0)
        throw std::runtime_error("PostgreSQL target must be empty; migration refused");
}

struct Column
{
    QString name;
    QString type;
};

std::vector<Column> targetColumns(QSqlDatabase& database, const QString& table)
{
    QSqlQuery query(database);
    query.prepare(
        QStringLiteral("SELECT column_name,data_type FROM information_schema.columns "
                       "WHERE table_schema='public' AND table_name=? ORDER BY ordinal_position"));
    query.addBindValue(table);
    if (!query.exec())
        throw std::runtime_error(errorText(query));
    std::vector<Column> columns;
    while (query.next())
        columns.push_back({query.value(0).toString(), query.value(1).toString()});
    return columns;
}

std::vector<QString> sourceColumns(QSqlDatabase& database, const QString& table)
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA table_info(\"") + table + QStringLiteral("\")")))
        throw std::runtime_error(errorText(query));
    std::vector<QString> columns;
    while (query.next())
        columns.push_back(query.value(1).toString());
    return columns;
}

std::int64_t copyTable(QSqlDatabase& source, QSqlDatabase& target, const QString& table)
{
    const auto pgColumns = targetColumns(target, table);
    const auto sqliteColumns = sourceColumns(source, table);
    if (pgColumns.empty() || sqliteColumns.size() != pgColumns.size())
        throw std::runtime_error("schema column count mismatch for " + table.toStdString());
    QStringList quoted;
    QStringList placeholders;
    for (std::size_t i = 0; i < pgColumns.size(); ++i)
    {
        if (sqliteColumns[i] != pgColumns[i].name)
            throw std::runtime_error("schema column mismatch for " + table.toStdString());
        quoted.push_back(QStringLiteral("\"") + pgColumns[i].name + QStringLiteral("\""));
        placeholders.push_back(QStringLiteral("?"));
    }

    QSqlQuery read(source);
    if (!read.exec(QStringLiteral("SELECT ") + quoted.join(',') + QStringLiteral(" FROM \"") +
                   table + QStringLiteral("\"")))
        throw std::runtime_error(errorText(read));
    QSqlQuery write(target);
    const QString insert = QStringLiteral("INSERT INTO \"") + table + QStringLiteral("\"(") +
                           quoted.join(',') + QStringLiteral(") VALUES(") + placeholders.join(',') +
                           QStringLiteral(")");
    if (!write.prepare(insert))
        throw std::runtime_error(errorText(write));

    std::int64_t count = 0;
    while (read.next())
    {
        for (int i = 0; i < static_cast<int>(pgColumns.size()); ++i)
        {
            QVariant value = read.value(i);
            if (!value.isNull() &&
                pgColumns[static_cast<std::size_t>(i)].type == QStringLiteral("boolean"))
                value = QVariant(value.toLongLong() != 0);
            // Legacy SQLite snapshot paths are not valid PostgreSQL archives. Keep the
            // audit row, but make it impossible to mistake the artifact for restorable PG data.
            if (table == QStringLiteral("backup_record") &&
                pgColumns[static_cast<std::size_t>(i)].name == QStringLiteral("storage_path"))
                value = QStringLiteral("legacy-sqlite://unavailable");
            if (table == QStringLiteral("backup_record") &&
                pgColumns[static_cast<std::size_t>(i)].name ==
                    QStringLiteral("verification_status"))
                value = QStringLiteral("LEGACY_SQLITE");
            write.bindValue(i, value);
        }
        if (!write.exec())
            throw std::runtime_error(errorText(write));
        write.finish();
        ++count;
    }
    if (read.lastError().isValid())
        throw std::runtime_error("SQLite source read failed");
    if (count !=
        scalar(source, QStringLiteral("SELECT COUNT(*) FROM \"") + table + QStringLiteral("\"")))
        throw std::runtime_error("source row-count verification failed for " + table.toStdString());
    return count;
}

void synchronizeIdentities(QSqlDatabase& target)
{
    for (const char* tableName : kIdentityTables)
    {
        const QString table = QString::fromUtf8(tableName);
        const QString column = table == QStringLiteral("flow_queue") ? QStringLiteral("sequence")
                                                                     : QStringLiteral("id");
        const QString sql =
            QStringLiteral("SELECT setval(pg_get_serial_sequence('%1','%2'),"
                           "COALESCE(MAX(\"%2\"),1),MAX(\"%2\") IS NOT NULL) FROM \"%1\"")
                .arg(table, column);
        execute(target, sql);
    }
}

void migrate(QSqlDatabase& source, QSqlDatabase& target, const PostgresConfig& config)
{
    execute(target, QStringLiteral("BEGIN"));
    try
    {
        execute(target, QStringLiteral("SELECT pg_advisory_xact_lock(56435350474)"));
        execute(target, QStringLiteral("SET LOCAL search_path TO public"));
        verifyFreshTarget(target);
        ncs::infrastructure::postgres::applyPostgresMigrations(&target, config, false);
        // Only bootstrap rows created above in THIS transaction can be removed.
        // Never truncate a pre-existing target or cascade into unrelated objects.
        QStringList tables;
        for (const char* table : kTables)
            tables.push_back(QStringLiteral("\"") + QString::fromUtf8(table) +
                             QStringLiteral("\""));
        execute(target, QStringLiteral("TRUNCATE TABLE ") + tables.join(',') +
                            QStringLiteral(" RESTART IDENTITY"));

        for (const char* tableName : kTables)
        {
            const QString table = QString::fromUtf8(tableName);
            const std::int64_t copied = copyTable(source, target, table);
            const std::int64_t targetCount = scalar(
                target, QStringLiteral("SELECT COUNT(*) FROM \"") + table + QStringLiteral("\""));
            if (copied != targetCount)
                throw std::runtime_error("row-count verification failed for " +
                                         table.toStdString());
            std::cout << tableName << ": " << copied << '\n';
        }
        synchronizeIdentities(target);
        execute(target, QStringLiteral("COMMIT"));
    }
    catch (...)
    {
        try
        {
            execute(target, QStringLiteral("ROLLBACK"));
        }
        catch (...)
        {
        }
        throw;
    }
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try
    {
        const Options options = parse(argc, argv);
        Database source(QStringLiteral("ncs-sqlite-migration-source"), QStringLiteral("QSQLITE"));
        Database target(QStringLiteral("ncs-postgres-migration-target"), QStringLiteral("QPSQL"));
        openSource(source.get(), options);
        openTarget(target.get(), options);
        migrate(source.get(), target.get(), options.postgres);
        std::cout << "SQLite v10 data migrated to PostgreSQL v10 successfully\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "migration failed: " << error.what() << '\n';
        return 2;
    }
}
