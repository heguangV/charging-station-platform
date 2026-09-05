#include "server/controller/flow_routes.h"
#include "server/controller/navigation_routes.h"
#include "server/controller/station_routes.h"
#include "server/controller/user_identity_routes.h"
#include "server/controller/wallet_routes.h"
#include "server/server_app.h"

#include "core/application/bounded_executor.h"
#include "core/application/business_numbers.h"
#include "core/application/charge_flow_service.h"
#include "core/application/charging_repository.h"
#include "core/application/idempotency_service.h"
#include "core/application/in_memory_user_account_repository.h"
#include "core/application/navigation_service.h"
#include "core/application/station_service.h"
#include "core/application/user_identity_service.h"
#include "core/application/wallet_service.h"
#include "infrastructure/map/tencent_geocoder.h"
#include "infrastructure/map/tencent_route_planner.h"
#include "server/controller/api_routes.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <iostream>
#include <string>
#include <string_view>

namespace
{

class TestRunner final
{
  public:
    void check(const bool condition, const std::string_view message)
    {
        if (!condition)
        {
            std::cerr << "FAIL: " << message << '\n';
            ++failures_;
        }
    }
    int result() const
    {
        return failures_ == 0 ? 0 : 1;
    }

  private:
    int failures_ = 0;
};

QJsonObject envelope(const crow::response& response)
{
    return QJsonDocument::fromJson(QByteArray::fromStdString(response.body)).object();
}

crow::response call(ncs::server::ServerApp& app, const crow::HTTPMethod method, std::string url,
                    std::string body = {}, std::string token = {},
                    const std::string& idempotencyKey = {},
                    const std::string& contentType = "application/json; charset=utf-8")
{
    const auto question = url.find('?');
    const std::string path = url.substr(0, question);
    crow::request request(method, url, path, crow::query_string(url), {}, body, 1, 1, true, false,
                          false);
    request.remote_ip_address = "127.0.0.1";
    if (!contentType.empty())
        request.add_header("Content-Type", contentType);
    if (!token.empty())
        request.add_header("Authorization", "Bearer " + token);
    if (!idempotencyKey.empty())
        request.add_header("Idempotency-Key", idempotencyKey);
    crow::response response;
    app.handle_full(request, response);
    return response;
}

std::string smsCode(ncs::server::ServerApp& app, const std::string_view phone,
                    const std::string_view purpose)
{
    const auto response = call(app, crow::HTTPMethod::POST, "/api/v1/user/auth/sms/code",
                               "{\"phone\":\"" + std::string(phone) + "\",\"purpose\":\"" +
                                   std::string(purpose) + "\"}");
    return envelope(response)
        .value(QStringLiteral("data"))
        .toObject()
        .value(QStringLiteral("developmentCode"))
        .toString()
        .toStdString();
}

const char* kUuidA = "2cb640c6-6995-4be5-9161-f0e2c1210001";
const char* kUuidB = "2cb640c6-6995-4be5-9161-f0e2c1210002";
const char* kUuidC = "2cb640c6-6995-4be5-9161-f0e2c1210003";
const char* kUuidD = "2cb640c6-6995-4be5-9161-f0e2c1210004";
const char* kUuidE = "2cb640c6-6995-4be5-9161-f0e2c1210005";
const char* kUuidF = "2cb640c6-6995-4be5-9161-f0e2c1210006";

} // namespace

int main()
{
    using namespace ncs;
    TestRunner tests;
    core::application::SessionManager sessions;
    core::application::VerificationCodeService codes(true);
    core::application::InMemoryUserAccountRepository accounts;
    core::application::UserIdentityService identity(accounts, sessions, codes);
    core::application::BoundedExecutor blockingExecutor(2, 32);
    core::application::IdempotencyService idempotency;
    core::application::BusinessNumbers numbers;
    core::application::InMemoryChargingRepository repository;
    core::application::WalletService walletService(repository, accounts, numbers);
    const QString emptyMapKey;
    infrastructure::map::TencentGeocoder geocoder(emptyMapKey);
    infrastructure::map::TencentRoutePlanner routePlanner(emptyMapKey);
    core::application::StationService stationService(repository, geocoder);
    core::application::NavigationService navigationService(repository, geocoder, routePlanner);
    core::application::ChargeFlowService flowService(repository, accounts, accounts, numbers, 60);

    server::ServerApp app;
    server::controller::ApiRoutes api(app);
    server::controller::UserIdentityRoutes identityRoutes(api, identity, sessions, blockingExecutor,
                                                          true);
    server::controller::WalletRoutes walletRoutes(api, walletService, sessions, blockingExecutor,
                                                  idempotency);
    server::controller::StationRoutes stationRoutes(api, stationService, sessions,
                                                    blockingExecutor);
    server::controller::NavigationRoutes navigationRoutes(api, navigationService, sessions,
                                                          blockingExecutor);
    server::controller::FlowRoutes flowRoutes(api, flowService, sessions, blockingExecutor,
                                              idempotency);
    app.validate();

    const std::string registerCode = smsCode(app, "13800139001", "REGISTER");
    const auto registered = call(app, crow::HTTPMethod::POST, "/api/v1/user/auth/register",
                                 "{\"username\":\"business_user\",\"phone\":\"13800139001\","
                                 "\"password\":\"business-password\",\"smsCode\":\"" +
                                     registerCode + "\",\"deviceId\":\"business-terminal\"}");
    const std::string token = envelope(registered)
                                  .value(QStringLiteral("data"))
                                  .toObject()
                                  .value(QStringLiteral("accessToken"))
                                  .toString()
                                  .toStdString();
    if (registered.code != 201)
    {
        std::cerr << "DEBUG register code=" << registered.code << " body=" << registered.body
                  << "\n";
        std::cerr << "DEBUG sms body len=" << registerCode.size() << "\n";
    }
    tests.check(registered.code == 201 && token.size() >= 43, "user registered for business tests");

    tests.check(call(app, crow::HTTPMethod::GET, "/api/v1/user/wallet").code == 401,
                "wallet overview rejects anonymous access");

    tests.check(call(app, crow::HTTPMethod::POST, "/api/v1/user/wallet/recharges",
                     R"({"amountCent":5000})", token)
                        .code == 400,
                "recharge without an idempotency key is rejected");

    const auto recharge = call(app, crow::HTTPMethod::POST, "/api/v1/user/wallet/recharges",
                               R"({"amountCent":5000})", token, kUuidA);
    tests.check(recharge.code == 200 &&
                    envelope(recharge).value("data").toObject().value("balanceAfterCent").toInt() ==
                        5000,
                "virtual recharge credits the wallet");

    const auto replay = call(app, crow::HTTPMethod::POST, "/api/v1/user/wallet/recharges",
                             R"({"amountCent":5000})", token, kUuidA);
    tests.check(replay.code == 200 &&
                    envelope(replay).value("data").toObject().value("rechargeNo") ==
                        envelope(recharge).value("data").toObject().value("rechargeNo"),
                "idempotent recharge replays the original result");

    const auto conflict = call(app, crow::HTTPMethod::POST, "/api/v1/user/wallet/recharges",
                               R"({"amountCent":3000})", token, kUuidA);
    tests.check(conflict.code == 409 && envelope(conflict).value("code").toInt() == 14,
                "reused idempotency key with a different body conflicts");

    const auto overview = call(app, crow::HTTPMethod::GET, "/api/v1/user/wallet", {}, token);
    tests.check(overview.code == 200 &&
                    envelope(overview).value("data").toObject().value("balanceCent").toInt() ==
                        5000,
                "wallet overview reports the balance");

    const auto transactions =
        call(app, crow::HTTPMethod::GET, "/api/v1/user/wallet/transactions", {}, token);
    tests.check(
        transactions.code == 200 &&
            envelope(transactions).value("data").toObject().value("items").toArray().size() == 1 &&
            envelope(transactions).value("data").toObject().value("total").toInt() == 1,
        "wallet transactions list the recharge");
    tests.check(
        call(app, crow::HTTPMethod::GET, "/api/v1/user/wallet/transactions?type=CHARGE", {}, token)
                    .code == 200 &&
            envelope(call(app, crow::HTTPMethod::GET,
                          "/api/v1/user/wallet/transactions?type=CHARGE", {}, token))
                    .value("data")
                    .toObject()
                    .value("total")
                    .toInt() == 0,
        "transaction type filter narrows the ledger");
    tests.check(
        call(app, crow::HTTPMethod::GET, "/api/v1/user/wallet/transactions?type=NOPE", {}, token)
                .code == 422,
        "unknown transaction type is rejected");
    tests.check(
        call(app, crow::HTTPMethod::GET, "/api/v1/user/wallet/transactions?pageSize=101", {}, token)
                .code == 422,
        "page size above the cap is rejected");
    tests.check(call(app, crow::HTTPMethod::GET,
                     "/api/v1/user/wallet/transactions?fromAt=not-a-number", {}, token)
                        .code == 422,
                "malformed wallet timestamp is rejected");

    const auto stations = call(app, crow::HTTPMethod::GET, "/api/v1/user/stations", {}, token);
    const QJsonObject stationsData = envelope(stations).value("data").toObject();
    tests.check(stations.code == 200 && stationsData.value("locationFallback").toBool() &&
                    stationsData.value("items").toArray().size() >= 2,
                "nearby stations use the demo default location with a fallback flag");
    const QJsonObject firstStation = stationsData.value("items").toArray().first().toObject();
    tests.check(firstStation.contains(QStringLiteral("distanceMeter")) &&
                    firstStation.contains(QStringLiteral("idleCount")) &&
                    firstStation.value("totalPriceCentPerKwh").toInt() > 0,
                "station summaries include distance and price columns");

    tests.check(call(app, crow::HTTPMethod::GET, "/api/v1/user/stations/1", {}, token).code == 200,
                "station detail is reachable");
    tests.check(call(app, crow::HTTPMethod::GET, "/api/v1/user/stations/999", {}, token).code ==
                    404,
                "unknown station is not found");
    const auto chargers =
        call(app, crow::HTTPMethod::GET, "/api/v1/user/stations/1/chargers?status=0", {}, token);
    tests.check(chargers.code == 200 &&
                    envelope(chargers).value("data").toObject().value("items").toArray().size() >=
                        3,
                "charger list filters by status");
    tests.check(envelope(chargers)
                    .value("data")
                    .toObject()
                    .value("items")
                    .toArray()
                    .first()
                    .toObject()
                    .contains(QStringLiteral("totalCount")),
                "charger list exposes the documented cumulative count");
    const auto quote =
        call(app, crow::HTTPMethod::GET, "/api/v1/user/stations/1/quote?chargerType=1", {}, token);
    tests.check(
        quote.code == 200 &&
            envelope(quote).value("data").toObject().value("totalPriceCentPerKwh").toInt() == 135,
        "station quote returns the regional price breakdown");
    tests.check(call(app, crow::HTTPMethod::GET, "/api/v1/user/stations/1/quote", {}, token).code ==
                    422,
                "quote without a charger type is rejected");
    tests.check(
        call(app, crow::HTTPMethod::GET, "/api/v1/user/stations?latitudeE6=39977680", {}, token)
                .code == 422,
        "a partial coordinate pair is rejected");
    const auto route =
        call(app, crow::HTTPMethod::GET,
             "/api/v1/user/stations/1/route?latitudeE6=39977680&longitudeE6=116316417&mode=driving",
             {}, token);
    const QJsonObject routeData = envelope(route).value("data").toObject();
    tests.check(route.code == 200 &&
                    routeData.value("provider").toString() == QStringLiteral("LOCAL_FALLBACK") &&
                    routeData.value("routeFallback").toBool() &&
                    routeData.value("browserUrl")
                        .toString()
                        .startsWith(QStringLiteral("https://apis.map.qq.com/uri/v1/routeplan")),
                "route endpoint degrades safely when Tencent is unavailable");
    tests.check(
        call(app, crow::HTTPMethod::GET, "/api/v1/user/stations/1/route?mode=flying", {}, token)
                .code == 422,
        "route endpoint rejects unsupported travel modes");
    tests.check(call(app, crow::HTTPMethod::GET,
                     "/api/v1/user/stations/1/route?mode=driving&keyword=" + std::string(601, 'a'),
                     {}, token)
                        .code == 422,
                "route endpoint rejects oversized location keywords");
    tests.check(
        call(app, crow::HTTPMethod::GET, "/api/v1/user/stations/1/route?mode=walking").code == 401,
        "route endpoint requires a user session");
    tests.check(call(app, crow::HTTPMethod::GET, "/api/v1/user/stations/1/chargers?status=broken",
                     {}, token)
                        .code == 422,
                "malformed charger status is rejected");

    const auto flowCreated =
        call(app, crow::HTTPMethod::POST, "/api/v1/user/flows",
             R"({"stationId":1,"chargerType":1,"preferredChargerId":null})", token, kUuidB);
    const QJsonObject flowData = envelope(flowCreated).value("data").toObject();
    const std::string flowNo = flowData.value("flowNo").toString().toStdString();
    tests.check(flowCreated.code == 200 && flowData.value("status").toInt() == 20 &&
                    flowData.value("quote").toObject().contains(QStringLiteral("quoteNo")),
                "flow request allocates a device and returns a quote");

    const auto active = call(app, crow::HTTPMethod::GET, "/api/v1/user/flows/active", {}, token);
    tests.check(active.code == 200 &&
                    envelope(active).value("data").toObject().value("hasActiveFlow").toBool(),
                "active flow endpoint restores the pending flow");

    tests.check(call(app, crow::HTTPMethod::POST, "/api/v1/user/flows",
                     R"({"stationId":1,"chargerType":1})", token, kUuidC)
                        .code == 409,
                "a second flow request conflicts with the active one");

    const int flowVersion = flowData.value("version").toInt();
    const std::string quoteNo = flowData.value("quote")
                                    .toObject()
                                    .value(QStringLiteral("quoteNo"))
                                    .toString()
                                    .toStdString();
    tests.check(call(app, crow::HTTPMethod::POST,
                     "/api/v1/user/flows/" + flowNo + "/quote-confirmations",
                     R"({"quoteNo":")" + quoteNo + R"(","flowVersion":99})", token, kUuidC)
                        .code == 409,
                "stale flow version conflicts on confirmation");

    const auto confirmation = call(
        app, crow::HTTPMethod::POST, "/api/v1/user/flows/" + flowNo + "/quote-confirmations",
        R"({"quoteNo":")" + quoteNo + R"(","flowVersion":)" + std::to_string(flowVersion) + "}",
        token, kUuidE);
    const QJsonObject confirmationData = envelope(confirmation).value("data").toObject();
    const std::string orderNo = confirmationData.value("orderNo").toString().toStdString();
    tests.check(confirmation.code == 200 && confirmationData.value("status").toInt() == 30 &&
                    orderNo.size() >= 12,
                "quote confirmation creates the order and reserves the device");

    const auto start =
        call(app, crow::HTTPMethod::POST, "/api/v1/user/flows/" + flowNo + "/start",
             R"({"flowVersion":)" + std::to_string(confirmationData.value("version").toInt()) + "}",
             token, kUuidD);
    if (start.code != 200)
    {
        std::cerr << "DEBUG start code=" << start.code << " body=" << start.body << "\n";
    }
    tests.check(start.code == 200 &&
                    envelope(start).value("data").toObject().value("status").toInt() == 40,
                "start enters charging");

    const auto progress =
        call(app, crow::HTTPMethod::GET, "/api/v1/user/flows/" + flowNo + "/progress", {}, token);
    tests.check(
        progress.code == 200 &&
            envelope(progress).value("data").toObject().contains(QStringLiteral("simulatedSoc")) &&
            envelope(progress).value("data").toObject().value("powerWatt").toInt() == 60000,
        "progress exposes live simulated values");

    const auto settle =
        call(app, crow::HTTPMethod::POST, "/api/v1/user/flows/" + flowNo + "/settlements",
             R"({"flowVersion":99,"reasonCode":"USER_STOPPED"})", token, kUuidD);
    tests.check(settle.code == 409, "stale settlement version conflicts");

    const int startVersion = envelope(start).value("data").toObject().value("version").toInt();
    const auto retrySettle =
        call(app, crow::HTTPMethod::POST, "/api/v1/user/flows/" + flowNo + "/settlements",
             R"({"reasonCode":"USER_STOPPED","flowVersion":)" + std::to_string(startVersion) + "}",
             token, kUuidF);
    tests.check(retrySettle.code == 200 &&
                    envelope(retrySettle).value("data").toObject().value("status").toInt() == 60,
                "settlement succeeds with the current version");
    const QJsonObject receipt =
        envelope(call(app, crow::HTTPMethod::GET, "/api/v1/user/orders/" + orderNo, {}, token))
            .value("data")
            .toObject();
    tests.check(receipt.contains(QStringLiteral("amountCent")) &&
                    receipt.contains(QStringLiteral("energyMwh")),
                "order receipt returns the settlement snapshot");
    const auto orders = call(app, crow::HTTPMethod::GET, "/api/v1/user/orders", {}, token);
    tests.check(orders.code == 200 &&
                    envelope(orders).value("data").toObject().value("total").toInt() >= 1,
                "order list contains the settled order");
    tests.check(
        call(app, crow::HTTPMethod::GET, "/api/v1/user/orders?status=bad", {}, token).code == 422,
        "malformed order status is rejected");
    const auto activeAfterSettle =
        call(app, crow::HTTPMethod::GET, "/api/v1/user/flows/active", {}, token);
    tests.check(
        envelope(activeAfterSettle).value("data").toObject().value("hasActiveFlow").toBool() ==
            false,
        "settled flow no longer counts as active");
    return tests.result();
}
