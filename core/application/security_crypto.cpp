#include "core/application/security_crypto.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <charconv>
#include <stdexcept>
#include <vector>

namespace ncs::core::application {
namespace {

std::vector<unsigned char> randomBytes(const std::size_t size)
{
    std::vector<unsigned char> bytes(size);
    if (size == 0 || RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("secure random generation failed");
    }
    return bytes;
}

std::string base64UrlEncode(const unsigned char *data, const std::size_t size)
{
    std::string encoded(4 * ((size + 2) / 3), '\0');
    const int length = EVP_EncodeBlock(
        reinterpret_cast<unsigned char *>(encoded.data()), data, static_cast<int>(size));
    if (length < 0) {
        throw std::runtime_error("base64 encoding failed");
    }
    encoded.resize(static_cast<std::size_t>(length));
    for (char &character : encoded) {
        if (character == '+') character = '-';
        if (character == '/') character = '_';
    }
    while (!encoded.empty() && encoded.back() == '=') encoded.pop_back();
    return encoded;
}

std::vector<unsigned char> base64UrlDecode(std::string encoded)
{
    for (char &character : encoded) {
        if (character == '-') character = '+';
        if (character == '_') character = '/';
    }
    while (encoded.size() % 4 != 0) encoded.push_back('=');
    std::vector<unsigned char> decoded((encoded.size() / 4) * 3);
    const int length = EVP_DecodeBlock(
        decoded.data(),
        reinterpret_cast<const unsigned char *>(encoded.data()),
        static_cast<int>(encoded.size()));
    if (length < 0) return {};
    std::size_t padding = 0;
    if (!encoded.empty() && encoded.back() == '=') ++padding;
    if (encoded.size() > 1 && encoded[encoded.size() - 2] == '=') ++padding;
    decoded.resize(static_cast<std::size_t>(length) - padding);
    return decoded;
}

std::string hexEncode(const unsigned char *data, const std::size_t size)
{
    static constexpr char alphabet[] = "0123456789abcdef";
    std::string result(size * 2, '\0');
    for (std::size_t index = 0; index < size; ++index) {
        result[index * 2] = alphabet[data[index] >> 4];
        result[index * 2 + 1] = alphabet[data[index] & 0x0f];
    }
    return result;
}

} // namespace

std::string secureRandomToken(const std::size_t bytes)
{
    const auto random = randomBytes(bytes);
    return base64UrlEncode(random.data(), random.size());
}

std::string sha256Hex(const std::string_view value)
{
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0;
    if (EVP_Digest(
            value.data(), value.size(), digest.data(), &size, EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("SHA-256 digest failed");
    }
    return hexEncode(digest.data(), size);
}

std::string PasswordHasher::hash(
    const std::string_view password, const int iterations,
    const std::size_t minimumPasswordLength) const
{
    if (password.size() < minimumPasswordLength || password.size() > 128) {
        throw std::invalid_argument("password length is outside the accepted range");
    }
    if (iterations < minimumIterations || iterations > maximumIterations) {
        throw std::invalid_argument("iteration count is outside the accepted range");
    }
    const auto salt = randomBytes(16);
    std::array<unsigned char, 32> derived{};
    if (PKCS5_PBKDF2_HMAC(
            password.data(), static_cast<int>(password.size()),
            salt.data(), static_cast<int>(salt.size()),
            iterations, EVP_sha256(),
            static_cast<int>(derived.size()), derived.data()) != 1) {
        throw std::runtime_error("password hashing failed");
    }
    return "pbkdf2-sha256$" + std::to_string(iterations) + "$"
        + base64UrlEncode(salt.data(), salt.size()) + "$"
        + base64UrlEncode(derived.data(), derived.size());
}

bool PasswordHasher::needsRehash(const std::string_view encodedHash)
{
    const auto first = encodedHash.find('$');
    const auto second = encodedHash.find('$', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos
        || encodedHash.substr(0, first) != "pbkdf2-sha256") {
        return false;
    }
    const auto iterationText = encodedHash.substr(first + 1, second - first - 1);
    int iterations = 0;
    const auto parsed = std::from_chars(
        iterationText.data(), iterationText.data() + iterationText.size(), iterations);
    return parsed.ec == std::errc{} && parsed.ptr == iterationText.data() + iterationText.size()
        && iterations >= minimumIterations && iterations < currentIterations;
}

bool PasswordHasher::verify(
    const std::string_view password,
    const std::string_view encodedHash) const
{
    const auto first = encodedHash.find('$');
    const auto second = encodedHash.find('$', first + 1);
    const auto third = encodedHash.find('$', second + 1);
    if (first == std::string_view::npos || second == std::string_view::npos
        || third == std::string_view::npos
        || encodedHash.substr(0, first) != "pbkdf2-sha256") {
        return false;
    }
    int iterations = 0;
    const auto iterationText = encodedHash.substr(first + 1, second - first - 1);
    const auto parsed = std::from_chars(
        iterationText.data(), iterationText.data() + iterationText.size(), iterations);
    if (parsed.ec != std::errc{} || parsed.ptr != iterationText.data() + iterationText.size()
        || iterations < minimumIterations || iterations > maximumIterations) {
        return false;
    }
    const auto salt = base64UrlDecode(std::string(
        encodedHash.substr(second + 1, third - second - 1)));
    const auto expected = base64UrlDecode(std::string(encodedHash.substr(third + 1)));
    if (salt.size() < 16 || expected.size() != 32) return false;

    std::array<unsigned char, 32> actual{};
    if (PKCS5_PBKDF2_HMAC(
            password.data(), static_cast<int>(password.size()),
            salt.data(), static_cast<int>(salt.size()), iterations, EVP_sha256(),
            static_cast<int>(actual.size()), actual.data()) != 1) {
        return false;
    }
    return CRYPTO_memcmp(actual.data(), expected.data(), actual.size()) == 0;
}

} // namespace ncs::core::application
