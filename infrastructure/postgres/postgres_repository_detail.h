#pragma once

#include "infrastructure/postgres/postgres_repository.h"
#include "infrastructure/postgres/postgres_seed.h"

#include "core/application/security_crypto.h"

#include <openssl/evp.h>

#include <QByteArray>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace ncs::infrastructure::postgres::detail
{

using namespace ncs::core::application;

constexpr int kLatestSchemaVersion = 10;
constexpr const char* kLatestSchemaChecksum = "ncs-v10-order-confirmation";

inline bool durableWalSettings(const std::string_view fsync, const std::string_view fullPageWrites,
                               const std::string_view synchronousCommit)
{
    return fsync == "on" && fullPageWrites == "on" &&
           (synchronousCommit == "on" || synchronousCommit == "local" ||
            synchronousCommit == "remote_write" || synchronousCommit == "remote_apply");
}

inline std::string sqlError(const QSqlQuery&)
{
    // PostgreSQL DETAIL may contain full business rows, keys or SQL text.
    return "database statement failed";
}

// 连接封装：每次工作从 Qt SQL 打开一个线程私有 QPSQL 连接。连接名随机且仅在本进程
// 使用；凭据通过独立字段传给驱动，不拼接、不记录数据库 URL。
class Connection final
{
  public:
    explicit Connection(const PostgresConfig& config, QSemaphore* connectionSlots)
        : connectionSlots_(connectionSlots)
    {
        connectionSlots_->acquire();
        static std::atomic<std::uint64_t> sequence{0};
        connectionName_ = QStringLiteral("ncs-pg-%1-%2")
                              .arg(reinterpret_cast<quintptr>(this), 0, 16)
                              .arg(sequence.fetch_add(1, std::memory_order_relaxed));
        database_ = QSqlDatabase::addDatabase(QStringLiteral("QPSQL"), connectionName_);
        database_.setHostName(QString::fromStdString(config.host));
        database_.setPort(config.port);
        database_.setDatabaseName(QString::fromStdString(config.database));
        database_.setUserName(QString::fromStdString(config.user));
        database_.setPassword(QString::fromStdString(config.password));
        QString options = QStringLiteral("connect_timeout=%1;sslmode=%2")
                              .arg(config.connectTimeoutSeconds)
                              .arg(QString::fromStdString(config.sslMode));
        if (!config.sslRootCertificate.empty())
        {
            options += QStringLiteral(";sslrootcert=%1")
                           .arg(QString::fromStdString(config.sslRootCertificate));
        }
        database_.setConnectOptions(options);
        if (!database_.open())
        {
            release();
            throw std::runtime_error("database unavailable: PostgreSQL connection failed");
        }
        try
        {
            execute("SET application_name = 'ncs_server'");
            execute("SET lock_timeout = '5s'");
            execute("SET statement_timeout = '30s'");
        }
        catch (...)
        {
            release();
            throw;
        }
    }

    ~Connection()
    {
        release();
    }

    QSqlDatabase* get()
    {
        return &database_;
    }

    void execute(const char* sql) const
    {
        QSqlQuery query(database_);
        if (!query.exec(QString::fromUtf8(sql)))
            throw std::runtime_error(sqlError(query));
    }

  private:
    void release()
    {
        if (connectionSlots_ == nullptr)
            return;
        database_.close();
        database_ = {};
        QSqlDatabase::removeDatabase(connectionName_);
        connectionSlots_->release();
        connectionSlots_ = nullptr;
    }

    QString connectionName_;
    QSqlDatabase database_;
    QSemaphore* connectionSlots_ = nullptr;
};

// 语句封装：prepare/bind/step 每步都校验返回码并抛出，杜绝静默失败。
// 所有 SQL 都通过 bind 参数化，值不进 SQL 文本（防注入的第一道防线）。
class Statement final
{
  public:
    Statement(QSqlDatabase* database, const char* sql) : query_(*database)
    {
        if (!query_.prepare(QString::fromUtf8(sql)))
            throw std::runtime_error(sqlError(query_));
    }

    ~Statement() = default;

    void bind(const int index, const std::int64_t value)
    {
        bindValue(index, QVariant::fromValue<qlonglong>(value));
    }

    void bind(const int index, const int value)
    {
        bindValue(index, value);
    }

    void bind(const int index, const double value)
    {
        bindValue(index, value);
    }

    void bind(const int index, const std::string_view value)
    {
        bindValue(index, QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())));
    }

    void bindNull(const int index)
    {
        bindValue(index, QVariant());
    }

    void bindBlob(const int index, const std::vector<unsigned char>& value)
    {
        bindValue(index, QByteArray(reinterpret_cast<const char*>(value.data()),
                                    static_cast<qsizetype>(value.size())));
    }

    bool row()
    {
        ensureExecuted();
        return query_.next();
    }

    void execute()
    {
        ensureExecuted();
    }

    void reset()
    {
        query_.finish();
        executed_ = false;
    }

    std::int64_t integer(const int column) const
    {
        return query_.value(column).toLongLong();
    }

    double real(const int column) const
    {
        return query_.value(column).toDouble();
    }

    std::string text(const int column) const
    {
        const auto value = query_.value(column).toString().toUtf8();
        return {value.constData(), static_cast<std::size_t>(value.size())};
    }

    bool isNull(const int column) const
    {
        return query_.value(column).isNull();
    }

    std::vector<unsigned char> blob(const int column) const
    {
        const QByteArray value = query_.value(column).toByteArray();
        if (value.isEmpty())
            return {};
        return {reinterpret_cast<const unsigned char*>(value.constData()),
                reinterpret_cast<const unsigned char*>(value.constData()) + value.size()};
    }

    std::int64_t rowsAffected() const
    {
        return query_.numRowsAffected();
    }

  private:
    void bindValue(const int index, const QVariant& value)
    {
        // Repository SQL uses positional placeholders; never infer binding mode
        // from literals or comments in the SQL text.
        query_.bindValue(index - 1, value);
    }

    void ensureExecuted()
    {
        if (executed_)
            return;
        if (!query_.exec())
            throw std::runtime_error(sqlError(query_));
        executed_ = true;
    }

    QSqlQuery query_;
    bool executed_ = false;
};

// 线程本地事务上下文：withTransaction() 打开连接并登记在此，
// 事务内嵌套调用的仓储方法经 useDatabase() 复用同一连接，
// 从而把多个仓储写入并入应用服务声明的一个原生事务。
struct TransactionContext
{
    const PostgresRepository* owner = nullptr;
    QSqlDatabase* database = nullptr;
    bool writable = false;
};

inline thread_local TransactionContext transactionContext;

inline bool inWriteTransaction(const PostgresRepository* owner)
{
    return transactionContext.owner == owner && transactionContext.writable;
}

inline std::string forUpdate(const PostgresRepository* owner, const bool skipLocked = false)
{
    if (!inWriteTransaction(owner))
        return {};
    return skipLocked ? " FOR UPDATE SKIP LOCKED" : " FOR UPDATE";
}

inline void advisoryTransactionLock(QSqlDatabase* database, const std::string_view scope)
{
    Statement lock(database, "SELECT pg_advisory_xact_lock(hashtextextended(?,0))");
    lock.bind(1, scope);
    if (!lock.row())
        throw std::runtime_error("database advisory lock failed");
}

template <typename Work>
auto useDatabase(const PostgresRepository* owner, const PostgresConfig& config, Work&& work)
{
    if (transactionContext.owner == owner)
    {
        return work(transactionContext.database);
    }
    Connection connection(config, &owner->connectionSlots());
    return work(connection.get());
}

inline void execute(QSqlDatabase* database, const char* sql)
{
    QSqlQuery query(*database);
    if (!query.exec(QString::fromUtf8(sql)))
        throw std::runtime_error(sqlError(query));
}

inline void bindOptional(Statement& statement, const int index,
                         const std::optional<std::int64_t> value)
{
    if (value)
        statement.bind(index, *value);
    else
        statement.bindNull(index);
}

inline void bindOptional(Statement& statement, const int index,
                         const std::optional<std::string>& value)
{
    if (value)
        statement.bind(index, *value);
    else
        statement.bindNull(index);
}

inline std::optional<std::int64_t> optionalInteger(const Statement& statement, const int column)
{
    return statement.isNull(column) ? std::nullopt
                                    : std::optional<std::int64_t>(statement.integer(column));
}

inline std::optional<std::string> optionalText(const Statement& statement, const int column)
{
    return statement.isNull(column) ? std::nullopt
                                    : std::optional<std::string>(statement.text(column));
}

} // namespace ncs::infrastructure::postgres::detail
