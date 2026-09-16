#pragma once

#include <string_view>

namespace ncs::server::runtime
{
struct ServerConfig;

namespace detail
{

// Returns false for an option owned by another configuration section.
bool applyDatabaseSetting(ServerConfig& config, std::string_view option, std::string_view value,
                          std::string_view source);

} // namespace detail
} // namespace ncs::server::runtime
