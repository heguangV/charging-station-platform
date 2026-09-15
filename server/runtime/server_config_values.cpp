#include "server/runtime/server_config_values.h"

#include "server/runtime/server_config.h"

#include <QFileInfo>
#include <QString>

#include <charconv>

namespace ncs::server::runtime::detail
{

std::string normalizePath(const std::string_view value, const std::string_view source)
{
    if (value.empty())
    {
        throw ConfigError("empty filesystem path for " + std::string(source));
    }
    const auto path = QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
    const auto bytes = QFileInfo(path).absoluteFilePath().toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

unsigned long parseUnsigned(const std::string_view value, const unsigned long minimum,
                            const unsigned long maximum, const std::string_view source)
{
    unsigned long parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (value.empty() || result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        parsed < minimum || parsed > maximum)
    {
        throw ConfigError("invalid numeric value for " + std::string(source));
    }
    return parsed;
}

} // namespace ncs::server::runtime::detail
