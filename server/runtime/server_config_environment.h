#pragma once

#include "server/runtime/server_config.h"

#include <optional>
#include <string>
#include <unordered_map>

namespace ncs::server::runtime::detail
{

using EnvironmentEntries = std::unordered_map<std::string, std::string>;

std::optional<EnvironmentEntries> loadEnvironmentFile(const ServerConfig& config,
                                                      const EnvironmentLookup& environmentLookup);

} // namespace ncs::server::runtime::detail
