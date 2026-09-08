// .env 文件加载内部辅助（server_config 的 detail 层）：解析 NCS_ENV_FILE
// 指定文件（缺失即报错）或资产目录默认 .env（缺失可容忍）。 键名做合法性校验、文件上限
// 64KiB；返回键值表供配置合并，进程环境变量随后覆盖同名文件值。
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
