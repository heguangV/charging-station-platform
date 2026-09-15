#pragma once

#include <string>
#include <string_view>

namespace ncs::server::runtime::detail
{

std::string normalizePath(std::string_view value, std::string_view source);
unsigned long parseUnsigned(std::string_view value, unsigned long minimum, unsigned long maximum,
                            std::string_view source);

} // namespace ncs::server::runtime::detail
