#include "net/api_client.h"

#include <QCoreApplication>
#include <QDebug>
#include <QNetworkReply>

namespace
{
int failures = 0;

void expect(bool condition, const char* message)
{
    if (!condition)
    {
        qCritical() << "FAILED:" << message;
        ++failures;
    }
}
} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    using ncs::user::ApiClient;

    const QByteArray expiredQuote =
        R"({"success":false,"code":16,"message":"quote expired","userMessage":"报价已过期，请重新排队或预约","data":null})";
    const auto businessError = ApiClient::parseEnvelope(409, expiredQuote,
                                                         QNetworkReply::ContentAccessDenied,
                                                         QStringLiteral("Error transferring"));
    expect(!businessError.ok(), "HTTP 409 must be an API failure");
    expect(businessError.code == QStringLiteral("16"), "business code must survive HTTP errors");
    expect(businessError.message == QStringLiteral("报价已过期，请重新排队或预约"),
           "userMessage must survive HTTP errors");

    const auto timeout = ApiClient::parseEnvelope(0, {}, QNetworkReply::TimeoutError,
                                                  QStringLiteral("Operation timed out"));
    expect(timeout.code == QStringLiteral("NetworkError"), "timeout must be a transport failure");
    expect(ApiClient::isRetryableTransportError(QNetworkReply::TimeoutError),
           "timeout must be retried once");
    expect(ApiClient::isRetryableTransportError(QNetworkReply::RemoteHostClosedError),
           "connection close must be retried once");
    expect(!ApiClient::isRetryableTransportError(QNetworkReply::ContentAccessDenied),
           "HTTP authorization failures must not be retried");

    if (failures == 0) qInfo() << "All user network tests passed.";
    return failures == 0 ? 0 : 1;
}
