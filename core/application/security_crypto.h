// 安全原语：密码学安全随机令牌、SHA-256 摘要与 PBKDF2-HMAC-SHA256 口令哈希/校验。
// 会话令牌生成（secureRandomToken）与用户/管理员口令存储（PasswordHasher）依赖本模块。
// 约束：当前标准为 60 万次迭代（OWASP 建议）；低于 10
// 万次的旧哈希在登录成功后透明重哈希（needsRehash）。

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace ncs::core::application
{

// 生成密码学安全随机令牌（bytes 字节，OpenSSL RAND），base64url 编码输出；随机源不可用时抛
// std::runtime_error。
std::string secureRandomToken(std::size_t bytes = 32);
// 计算 SHA-256 摘要并以小写十六进制返回（用于会话令牌/幂等请求摘要）；摘要失败时抛
// std::runtime_error。
std::string sha256Hex(std::string_view value);

class PasswordHasher final
{
  public:
    // OWASP 2026 recommendation for PBKDF2-HMAC-SHA256; hashes below this
    // iteration count are re-hashed transparently after a successful login.
    static constexpr int currentIterations = 600000;
    static constexpr int minimumIterations = 100000;
    static constexpr int maximumIterations = 2000000;

    // PBKDF2-HMAC-SHA256 口令哈希：输出格式 "pbkdf2-sha256$迭代数$salt$派生值"（16 字节随机盐）；
    // 密码长度须在 minimumPasswordLength~128、迭代数须在 minimum~maximumIterations，否则抛
    // std::invalid_argument。
    std::string hash(std::string_view password, int iterations = currentIterations,
                     std::size_t minimumPasswordLength = 10) const;
    // 恒时校验口令与编码哈希（CRYPTO_memcmp）：解析迭代数与盐后重算比对；格式非法、迭代数越界或长度不符一律返回
    // false。
    bool verify(std::string_view password, std::string_view encodedHash) const;
    // 重哈希判定：编码哈希格式合法且迭代数在 [minimumIterations, currentIterations) 时返回
    // true，供登录成功后透明升级到当前标准迭代数。
    static bool needsRehash(std::string_view encodedHash);
};

} // namespace ncs::core::application
