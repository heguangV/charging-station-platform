#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace ncs::core::application {

std::string secureRandomToken(std::size_t bytes = 32);
std::string sha256Hex(std::string_view value);

class PasswordHasher final {
public:
    // OWASP 2026 recommendation for PBKDF2-HMAC-SHA256; hashes below this
    // iteration count are re-hashed transparently after a successful login.
    static constexpr int currentIterations = 600000;
    static constexpr int minimumIterations = 100000;
    static constexpr int maximumIterations = 2000000;

    std::string hash(
        std::string_view password,
        int iterations = currentIterations,
        std::size_t minimumPasswordLength = 10) const;
    bool verify(std::string_view password, std::string_view encodedHash) const;
    static bool needsRehash(std::string_view encodedHash);
};

} // namespace ncs::core::application
