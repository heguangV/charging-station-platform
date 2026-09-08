// 启动前检查与 TLS 上下文：拒绝开发环境之外或非回环地址的明文 HTTP；校验证书/私钥 PEM
// 有效性、有效期、密钥匹配、文件权限与监听地址覆盖。 校验通过后由 createTlsContext 构建 asio SSL
// 上下文供 HTTPS 监听；任何失败抛 ConfigError 中止启动。
#pragma once

#include "server/runtime/server_config.h"

#include <asio/ssl/context.hpp>

namespace ncs::server::runtime
{

void runStartupChecks(const ServerConfig& config);
asio::ssl::context createTlsContext(const ServerConfig& config);

} // namespace ncs::server::runtime
