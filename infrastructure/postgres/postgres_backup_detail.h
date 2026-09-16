#pragma once

#include "infrastructure/postgres/postgres_repository_detail.h"

namespace ncs::infrastructure::postgres::detail
{

// 工具函数：备份文件 SHA-256 摘要与数据目录权限收紧（仅属主可读写）。
inline std::string fileSha256(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("backup file unavailable");
    EVP_MD_CTX* raw = EVP_MD_CTX_new();
    if (!raw)
        throw std::runtime_error("backup digest unavailable");
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(raw, &EVP_MD_CTX_free);
    if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("backup digest unavailable");
    std::array<char, 64 * 1024> buffer{};
    while (input)
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0 &&
            EVP_DigestUpdate(context.get(), buffer.data(), static_cast<std::size_t>(count)) != 1)
        {
            throw std::runtime_error("backup digest unavailable");
        }
    }
    if (!input.eof())
        throw std::runtime_error("backup file read failed");
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digestSize = 0;
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &digestSize) != 1)
        throw std::runtime_error("backup digest unavailable");
    static constexpr char hex[] = "0123456789abcdef";
    std::string encoded(digestSize * 2, '\0');
    for (unsigned int index = 0; index < digestSize; ++index)
    {
        encoded[index * 2] = hex[digest[index] >> 4];
        encoded[index * 2 + 1] = hex[digest[index] & 0x0f];
    }
    return encoded;
}

inline bool restrictOwnerPermissions(const std::filesystem::path& path, const bool directory)
{
    std::error_code error;
    const auto permissions =
        directory ? std::filesystem::perms::owner_all
                  : std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
    std::filesystem::permissions(path, permissions, std::filesystem::perm_options::replace, error);
#ifdef _WIN32
    (void)error;
    return true;
#else
    return !error;
#endif
}

inline bool runPostgresTool(const std::string& executable, const QStringList& arguments,
                            const PostgresConfig& config, const int timeoutMilliseconds)
{
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    if (!config.password.empty())
        environment.insert(QStringLiteral("PGPASSWORD"), QString::fromStdString(config.password));
    environment.insert(QStringLiteral("PGSSLMODE"), QString::fromStdString(config.sslMode));
    if (!config.sslRootCertificate.empty())
        environment.insert(QStringLiteral("PGSSLROOTCERT"),
                           QString::fromStdString(config.sslRootCertificate));
    process.setProcessEnvironment(environment);
    process.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    process.start(QString::fromStdString(executable), arguments);
    if (!process.waitForStarted(config.connectTimeoutSeconds * 1000) ||
        !process.waitForFinished(timeoutMilliseconds))
    {
        process.kill();
        process.waitForFinished();
        return false;
    }
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

} // namespace ncs::infrastructure::postgres::detail
