#include "server/runtime/server_config_environment.h"

#include <QFile>
#include <QFileInfo>
#include <QString>

#include <cctype>
#include <cstring>
#include <string_view>

namespace ncs::server::runtime::detail
{
namespace
{

// Environment files are tiny by design; anything larger is misconfiguration
// rather than configuration.
constexpr qint64 maximumEnvironmentFileBytes = 64 * 1024;

std::string utf8Path(const QString& path)
{
    const auto bytes = path.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

QString pathFromUtf8(const std::string_view value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

bool environmentKeyCharacter(const char character)
{
    const auto byte = static_cast<unsigned char>(character);
    return std::isalnum(byte) != 0 || character == '_';
}

bool validEnvironmentKey(const std::string_view key)
{
    if (key.empty())
        return false;
    const auto first = static_cast<unsigned char>(key.front());
    if (std::isalpha(first) == 0 && key.front() != '_')
        return false;
    for (const char character : key)
    {
        if (!environmentKeyCharacter(character))
            return false;
    }
    return true;
}

std::string_view trimmed(const std::string_view value)
{
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin])) != 0)
    {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0)
    {
        --end;
    }
    return value.substr(begin, end - begin);
}

// Parses dotenv-style KEY=VALUE text: blank lines, comment lines and lines
// without a well-formed key are skipped, an optional `export` prefix and
// surrounding quotes on values are accepted, and the first occurrence of a
// key wins. Values may be secrets, so callers must never include them in
// error messages or logs.
EnvironmentEntries parseEnvironmentFile(const QByteArray& content)
{
    EnvironmentEntries entries;
    const auto lines = content.split('\n');
    for (const QByteArray& rawLine : lines)
    {
        std::string_view line(rawLine.constData(), static_cast<std::size_t>(rawLine.size()));
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        const auto statement = trimmed(line);
        if (statement.empty() || statement.front() == '#')
            continue;
        if (statement.rfind("export", 0) == 0)
        {
            const auto suffix = statement.substr(std::strlen("export"));
            if (suffix.empty() || std::isspace(static_cast<unsigned char>(suffix.front())) == 0)
            {
                continue; // "export" must be its own token
            }
            line = trimmed(suffix);
        }
        else
        {
            line = statement;
        }
        const auto equals = line.find('=');
        if (equals == std::string_view::npos)
            continue;
        const auto key = trimmed(line.substr(0, equals));
        if (!validEnvironmentKey(key))
            continue;
        auto value = trimmed(line.substr(equals + 1));
        if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                                  (value.front() == '\'' && value.back() == '\'')))
        {
            value = value.substr(1, value.size() - 2);
        }
        entries.emplace(std::string(key), std::string(value));
    }
    return entries;
}

} // namespace

std::optional<EnvironmentEntries> loadEnvironmentFile(const ServerConfig& config,
                                                      const EnvironmentLookup& environmentLookup)
{
    const auto specified = environmentLookup("NCS_ENV_FILE");
    const bool explicitFile = specified && !specified->empty();
    QString path;
    if (explicitFile)
    {
        path = QFileInfo(pathFromUtf8(*specified)).absoluteFilePath();
    }
    else
    {
        path = QFileInfo(QString::fromUtf8(config.assetDirectory.data(),
                                           static_cast<qsizetype>(config.assetDirectory.size())) +
                         QStringLiteral("/.env"))
                   .absoluteFilePath();
    }
    if (!QFileInfo::exists(path))
    {
        if (explicitFile)
            throw ConfigError("environment file is missing: " + utf8Path(path));
        return std::nullopt;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        throw ConfigError("environment file is unreadable: " + utf8Path(path));
    }
    if (file.size() > maximumEnvironmentFileBytes)
    {
        throw ConfigError("environment file exceeds 64 KiB: " + utf8Path(path));
    }
    QByteArray content = file.readAll();
    if (content.startsWith("\xEF\xBB\xBF"))
        content.remove(0, 3);
    return parseEnvironmentFile(content);
}

} // namespace ncs::server::runtime::detail
