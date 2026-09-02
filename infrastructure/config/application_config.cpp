#include "config/application_config.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTextStream>

#include <optional>
#include <utility>

namespace ncs::infrastructure
{
namespace
{

using ncs::core::AppError;
using ncs::core::ErrorCode;

QHash<QString, QString> readEnvFile(const QString& path)
{
    QHash<QString, QString> values;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return values;
    }

    QTextStream stream(&file);
    while (!stream.atEnd())
    {
        QString line = stream.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
        {
            continue;
        }
        if (line.startsWith(QStringLiteral("export ")))
        {
            line = line.mid(7).trimmed();
        }
        const qsizetype separator = line.indexOf(QLatin1Char('='));
        if (separator <= 0)
        {
            continue;
        }
        QString value = line.mid(separator + 1).trimmed();
        if (value.size() >= 2 &&
            ((value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"'))) ||
             (value.startsWith(QLatin1Char('\'')) && value.endsWith(QLatin1Char('\'')))))
        {
            value = value.mid(1, value.size() - 2);
        }
        values.insert(line.left(separator).trimmed(), value);
    }
    return values;
}

QString setting(const QHash<QString, QString>& fileValues, const QProcessEnvironment& environment,
                const QString& name, const QString& fallback)
{
    if (environment.contains(name))
    {
        return environment.value(name);
    }
    return fileValues.value(name, fallback);
}

std::optional<bool> parseBool(const QString& value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("1") || normalized == QStringLiteral("true") ||
        normalized == QStringLiteral("yes") || normalized == QStringLiteral("on"))
    {
        return true;
    }
    if (normalized == QStringLiteral("0") || normalized == QStringLiteral("false") ||
        normalized == QStringLiteral("no") || normalized == QStringLiteral("off"))
    {
        return false;
    }
    return std::nullopt;
}

QString normalizedPath(const QString& value)
{
    return value.trimmed().isEmpty() ? QString() : QDir::cleanPath(value);
}

ncs::core::Result<ApplicationConfig> invalidConfig(const QString& diagnostic,
                                                   const QString& userMessage)
{
    return ncs::core::Result<ApplicationConfig>::failure(
        AppError{ErrorCode::ValidationFailed, diagnostic, userMessage, {}});
}

} // namespace

ncs::core::Result<ApplicationConfig> ApplicationConfig::load(const QString& envFilePath)
{
    const auto processEnvironment = QProcessEnvironment::systemEnvironment();
    QString configPath = envFilePath;
    if (configPath.isEmpty())
    {
        configPath = processEnvironment.value(QStringLiteral("NCS_ENV_FILE"));
    }
    if (configPath.isEmpty())
    {
        const QString currentCandidate = QDir::current().filePath(QStringLiteral(".env"));
        const QString applicationCandidate =
            QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral(".env"));
        configPath = QFile::exists(currentCandidate) ? currentCandidate : applicationCandidate;
    }
    configPath = QDir::cleanPath(configPath);
    const auto fileValues = readEnvFile(configPath);

    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    ApplicationConfig config;
    config.environment_ = setting(fileValues, processEnvironment, QStringLiteral("NCS_ENV"),
                                  QStringLiteral("development"));
    config.serverHost_ = setting(fileValues, processEnvironment, QStringLiteral("NCS_SERVER_HOST"),
                                 QStringLiteral("127.0.0.1"));
    config.databasePath_ =
        normalizedPath(setting(fileValues, processEnvironment, QStringLiteral("NCS_DATABASE_PATH"),
                               QDir(appData).filePath(QStringLiteral("data/charge_platform.db"))));
    config.logDirectory_ =
        normalizedPath(setting(fileValues, processEnvironment, QStringLiteral("NCS_LOG_DIR"),
                               QDir(appData).filePath(QStringLiteral("logs"))));
    config.tlsCertificatePath_ = normalizedPath(
        setting(fileValues, processEnvironment, QStringLiteral("NCS_TLS_CERT_PATH"), QString()));
    config.tlsPrivateKeyPath_ = normalizedPath(
        setting(fileValues, processEnvironment, QStringLiteral("NCS_TLS_KEY_PATH"), QString()));
    config.tencentMapWebKey_ = setting(fileValues, processEnvironment,
                                       QStringLiteral("NCS_TENCENT_MAP_WEB_KEY"), QString());
    config.tencentMapServiceKey_ = setting(
        fileValues, processEnvironment, QStringLiteral("NCS_TENCENT_MAP_SERVICE_KEY"), QString());

    bool portOk = false;
    const int port = setting(fileValues, processEnvironment, QStringLiteral("NCS_SERVER_PORT"),
                             QStringLiteral("8443"))
                         .toInt(&portOk);
    if (!portOk || port < 1 || port > 65535)
    {
        return invalidConfig(QStringLiteral("NCS_SERVER_PORT is outside 1..65535"),
                             QStringLiteral("服务端口配置无效"));
    }
    config.serverPort_ = static_cast<quint16>(port);

    bool multiplierOk = false;
    config.billingTimeMultiplier_ =
        setting(fileValues, processEnvironment, QStringLiteral("NCS_BILLING_TIME_MULTIPLIER"),
                QStringLiteral("60"))
            .toInt(&multiplierOk);
    if (!multiplierOk || config.billingTimeMultiplier_ < 1 || config.billingTimeMultiplier_ > 300)
    {
        return invalidConfig(QStringLiteral("NCS_BILLING_TIME_MULTIPLIER is outside 1..300"),
                             QStringLiteral("计费时间倍率配置无效"));
    }

    const auto insecure =
        parseBool(setting(fileValues, processEnvironment, QStringLiteral("NCS_ALLOW_INSECURE_HTTP"),
                          QStringLiteral("true")));
    if (!insecure.has_value())
    {
        return invalidConfig(QStringLiteral("NCS_ALLOW_INSECURE_HTTP is not a boolean"),
                             QStringLiteral("HTTP 安全模式配置无效"));
    }
    config.allowInsecureHttp_ = *insecure;

    const auto simulatedSms =
        parseBool(setting(fileValues, processEnvironment,
                          QStringLiteral("NCS_SIMULATED_SMS_ENABLED"), QStringLiteral("true")));
    const auto dashboardSnapshot = parseBool(
        setting(fileValues, processEnvironment, QStringLiteral("NCS_DASHBOARD_SNAPSHOT_ENABLED"),
                QStringLiteral("true")));
    if (!simulatedSms.has_value() || !dashboardSnapshot.has_value())
    {
        return invalidConfig(QStringLiteral("feature flag is not a boolean"),
                             QStringLiteral("功能开关配置无效"));
    }
    config.simulatedSmsEnabled_ = *simulatedSms;
    config.dashboardSnapshotEnabled_ = *dashboardSnapshot;

    const QStringList environments = {QStringLiteral("development"), QStringLiteral("test"),
                                      QStringLiteral("demo"), QStringLiteral("production")};
    if (!environments.contains(config.environment_))
    {
        return invalidConfig(QStringLiteral("NCS_ENV is not supported"),
                             QStringLiteral("运行环境配置无效"));
    }
    if (config.serverHost_.trimmed().isEmpty() || config.databasePath_.isEmpty() ||
        config.logDirectory_.isEmpty())
    {
        return invalidConfig(QStringLiteral("required path or host is empty"),
                             QStringLiteral("必要的服务配置不完整"));
    }
    if (config.environment_ == QStringLiteral("production") &&
        (config.allowInsecureHttp_ || config.tlsCertificatePath_.isEmpty() ||
         config.tlsPrivateKeyPath_.isEmpty() || config.simulatedSmsEnabled_))
    {
        return invalidConfig(QStringLiteral("production requires TLS certificate and private key"),
                             QStringLiteral("正式环境必须配置 HTTPS 并禁用模拟短信"));
    }

    return ncs::core::Result<ApplicationConfig>::success(std::move(config));
}

QStringList ApplicationConfig::safeSummary() const
{
    return {
        QStringLiteral("environment=%1").arg(environment_),
        QStringLiteral("server=%1:%2").arg(serverHost_).arg(serverPort_),
        QStringLiteral("transport=%1")
            .arg(allowInsecureHttp_ ? QStringLiteral("http-dev") : QStringLiteral("https")),
        QStringLiteral("billingMultiplier=%1").arg(billingTimeMultiplier_),
        QStringLiteral("dashboardSnapshot=%1")
            .arg(dashboardSnapshotEnabled_ ? QStringLiteral("enabled")
                                           : QStringLiteral("disabled")),
    };
}

} // namespace ncs::infrastructure
