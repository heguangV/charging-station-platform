#include "server/runtime/startup_checks.h"

#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>

#include <QByteArray>
#include <QFile>
#include <QFileDevice>
#include <QString>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cstddef>
#include <memory>
#include <string>

namespace ncs::server::runtime
{
namespace
{

constexpr qint64 maximumPemBytes = 1024 * 1024;

using BioPointer = std::unique_ptr<BIO, decltype(&BIO_free)>;
using CertificatePointer = std::unique_ptr<X509, decltype(&X509_free)>;
using PrivateKeyPointer = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

QString pathFromUtf8(const std::string& path)
{
    return QString::fromUtf8(path.data(), static_cast<qsizetype>(path.size()));
}

QByteArray readPemFile(const std::string& path, const char* description)
{
    QFile file(pathFromUtf8(path));
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > maximumPemBytes)
    {
        throw ConfigError(std::string(description) + " is empty, oversized, or unreadable");
    }
    return file.readAll();
}

CertificatePointer parseCertificate(const QByteArray& pem)
{
    BioPointer input(BIO_new_mem_buf(pem.constData(), static_cast<int>(pem.size())), &BIO_free);
    if (!input)
    {
        throw ConfigError("TLS certificate could not be loaded");
    }
    CertificatePointer certificate(PEM_read_bio_X509(input.get(), nullptr, nullptr, nullptr),
                                   &X509_free);
    if (!certificate)
    {
        throw ConfigError("TLS certificate is not valid PEM");
    }
    return certificate;
}

PrivateKeyPointer parsePrivateKey(const QByteArray& pem)
{
    BioPointer input(BIO_new_mem_buf(pem.constData(), static_cast<int>(pem.size())), &BIO_free);
    if (!input)
    {
        throw ConfigError("TLS private key could not be loaded");
    }
    PrivateKeyPointer privateKey(PEM_read_bio_PrivateKey(input.get(), nullptr, nullptr, nullptr),
                                 &EVP_PKEY_free);
    if (!privateKey)
    {
        throw ConfigError("TLS private key is not valid unencrypted PEM");
    }
    return privateKey;
}

void checkPrivateKeyPermissions(const ServerConfig& config)
{
#ifdef Q_OS_UNIX
    const auto permissions = QFile::permissions(pathFromUtf8(config.tlsPrivateKeyPath));
    constexpr auto unsafePermissions = QFileDevice::ReadGroup | QFileDevice::WriteGroup |
                                       QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                       QFileDevice::WriteOther | QFileDevice::ExeOther;
    if ((permissions & unsafePermissions) != 0)
    {
        throw ConfigError("TLS private key permissions must be restricted to its owner");
    }
#else
    (void)config;
#endif
}

void checkTlsMaterial(const ServerConfig& config)
{
    validateTlsFiles(config);
    checkPrivateKeyPermissions(config);

    const auto certificate =
        parseCertificate(readPemFile(config.tlsCertificatePath, "TLS certificate"));
    const auto privateKey =
        parsePrivateKey(readPemFile(config.tlsPrivateKeyPath, "TLS private key"));

    const int startsInFuture = X509_cmp_current_time(X509_get0_notBefore(certificate.get()));
    const int alreadyExpired = X509_cmp_current_time(X509_get0_notAfter(certificate.get()));
    if (startsInFuture >= 0 || alreadyExpired <= 0)
    {
        throw ConfigError("TLS certificate is not currently valid");
    }
    if (X509_check_private_key(certificate.get(), privateKey.get()) != 1)
    {
        throw ConfigError("TLS certificate and private key do not match");
    }
    if (X509_check_ip_asc(certificate.get(), config.listenAddress.c_str(), 0) != 1)
    {
        throw ConfigError("TLS certificate does not cover the configured listen address");
    }
}

void checkListenEndpoint(const ServerConfig& config)
{
    asio::error_code error;
    const auto address = asio::ip::make_address(config.listenAddress, error);
    if (error)
    {
        throw ConfigError("listen address is invalid");
    }

    asio::io_context context;
    asio::ip::tcp::acceptor acceptor(context);
    const asio::ip::tcp::endpoint endpoint(address, config.port);
    acceptor.open(endpoint.protocol(), error);
    if (!error)
    {
        acceptor.set_option(asio::socket_base::reuse_address(true), error);
    }
    if (!error)
    {
        acceptor.bind(endpoint, error);
    }
    if (error)
    {
        throw ConfigError("listen endpoint is unavailable");
    }
    acceptor.close(error);
}

} // namespace

void runStartupChecks(const ServerConfig& config)
{
    asio::error_code error;
    const auto address = asio::ip::make_address(config.listenAddress, error);
    if (config.allowInsecureHttp && (config.environment != DeploymentEnvironment::Development ||
                                     error || !address.is_loopback()))
    {
        throw ConfigError("insecure HTTP requires development and a loopback listen address");
    }
    if (!config.allowInsecureHttp)
    {
        checkTlsMaterial(config);
    }
    checkListenEndpoint(config);
}

asio::ssl::context createTlsContext(const ServerConfig& config)
{
    asio::ssl::context context(asio::ssl::context::tls_server);
    context.set_verify_mode(asio::ssl::verify_none);
    context.set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
                        asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 |
                        asio::ssl::context::no_tlsv1_1);
    context.use_certificate_chain_file(config.tlsCertificatePath);
    context.use_private_key_file(config.tlsPrivateKeyPath, asio::ssl::context::pem);
    if (SSL_CTX_set_min_proto_version(context.native_handle(), TLS1_2_VERSION) != 1 ||
        SSL_CTX_set_cipher_list(context.native_handle(), "ECDHE+AESGCM:ECDHE+CHACHA20") != 1 ||
        SSL_CTX_set_ciphersuites(
            context.native_handle(),
            "TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256") != 1)
    {
        throw ConfigError("TLS security policy could not be configured");
    }
    return context;
}

} // namespace ncs::server::runtime
