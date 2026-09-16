// Agent 控制器契约测试：POST /api/v1/user/agent/chat 的鉴权、参数校验、结构化响应与降级语义。
// 覆盖未授权、缺失/超长/空白 message、未知字段、location 成对与范围校验、coordinateType 与
// chargerType 取值校验、结构化解包（stations/pois/route/actions/tools/llmUsed/degraded）、
// 以及外部地图与 LLM 不可用时的降级结果。
// 包含顺序：Crow 必须先于 Qt 头文件解析，否则 Qt 的 signals 宏会破坏 Crow 自身的成员声明。
#include "server/controller/agent_controller.h"
#include "server/controller/api_routes.h"
#include "server/server_app.h"

#include "agent/agent_service.h"

#include "core/application/bounded_executor.h"
#include "core/application/charging_repository.h"
#include "core/application/llm_client.h"
#include "core/application/poi_service.h"
#include "core/application/session_manager.h"
#include "core/application/station_service.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

using ncs::core::application::Geocoder;
using ncs::core::application::LlmChatRequest;
using ncs::core::application::LlmChatResponse;
using ncs::core::application::LlmClient;
using ncs::core::application::PlannedRoute;
using ncs::core::application::PoiItem;
using ncs::core::application::PoiProvider;
using ncs::core::application::PoiQuery;
using ncs::core::application::RoutePlanner;
using ncs::core::application::RoutePoint;
using ncs::core::application::ServiceResult;
using ncs::core::application::TravelMode;
using ncs::core::domain::ErrorCode;

class FixedGeocoder final : public Geocoder
{
  public:
    std::optional<Location> resolve(const std::string&) override
    {
        return Location{39977680, 116316417};
    }
};

class SilentLlm final : public LlmClient
{
  public:
    bool available() const override
    {
        return false;
    }

    ServiceResult<LlmChatResponse> chat(const LlmChatRequest&) override
    {
        return {ErrorCode::ExternalServiceUnavailable, std::nullopt};
    }
};

class FixedPlanner final : public RoutePlanner
{
  public:
    std::optional<RoutePoint> normalized;
    std::optional<PlannedRoute> next;
    int normalizeCalls = 0;

    std::optional<RoutePoint> normalizeGps(RoutePoint) override
    {
        ++normalizeCalls;
        return normalized;
    }

    std::optional<PlannedRoute> plan(RoutePoint, RoutePoint, TravelMode) override
    {
        return next;
    }
};

class FakePoiProvider final : public PoiProvider
{
  public:
    std::vector<PoiItem> items;
    int calls = 0;

    bool available() const override
    {
        return true;
    }

    ServiceResult<std::vector<PoiItem>> search(const PoiQuery& query) override
    {
        ++calls;
        if (query.category == "商场")
            return {ErrorCode::ExternalServiceUnavailable, std::nullopt};
        return {ErrorCode::Ok, items};
    }
};

QJsonObject envelope(const crow::response& response)
{
    return QJsonDocument::fromJson(QByteArray::fromStdString(response.body)).object();
}

QJsonObject dataOf(const crow::response& response)
{
    return envelope(response).value(QStringLiteral("data")).toObject();
}

crow::response call(ncs::server::ServerApp& app, const std::string& body,
                    const std::string& token = {},
                    const std::string& path = "/api/v1/user/agent/chat")
{
    crow::request request(crow::HTTPMethod::POST, path, path, crow::query_string(path), {}, body, 1,
                          1, true, false, false);
    request.remote_ip_address = "127.0.0.1";
    if (!token.empty())
        request.add_header("Authorization", "Bearer " + token);
    request.add_header("Content-Type", "application/json; charset=utf-8");
    crow::response response;
    app.handle_full(request, response);
    return response;
}

} // namespace

int main()
{
    using namespace ncs;
    TestRunner tests;

    core::application::SessionManager sessions;
    core::application::InMemoryChargingRepository repository;
    FixedGeocoder geocoder;
    core::application::StationService stations(repository, geocoder);
    FixedPlanner planner;
    FakePoiProvider poiProvider;
    PoiItem poi;
    poi.id = "POI-1";
    poi.name = "星巴克(中关村店)";
    poi.category = "咖啡厅";
    poi.address = "北京市海淀区中关村大街 1 号";
    poi.latitudeE6 = 39978100;
    poi.longitudeE6 = 116316000;
    poi.distanceMeter = 210;
    poiProvider.items = {poi};
    SilentLlm llm;
    agent::AgentService agentService(stations, planner, poiProvider, llm);
    core::application::BoundedExecutor executor(2, 32);

    server::ServerApp app;
    server::controller::ApiRoutes api(app);
    server::controller::AgentController controller(api, agentService, sessions, executor);
    app.validate();

    const auto now = std::chrono::system_clock::now();
    const auto issued =
        sessions.issue("user:7", "agent-terminal", core::application::TokenKind::User,
                       {core::application::Role::User}, now, std::chrono::hours(24));
    const auto adminIssued =
        sessions.issue("admin:1", "agent-terminal", core::application::TokenKind::Administrator,
                       {core::application::Role::Operator}, now, std::chrono::hours(8));
    tests.check(issued.has_value() && adminIssued.has_value(), "test sessions must be issuable");
    const std::string token = issued.value().accessToken;
    const std::string adminToken = adminIssued.value().accessToken;

    const std::string validBody =
        R"({"message":"附近哪里有充电站","location":{"latitudeE6":39977680,"longitudeE6":116316417}})";

    // —— 鉴权 ——
    {
        const auto anonymous = call(app, validBody);
        tests.check(anonymous.code == 401, "the agent endpoint must require a Bearer token");
        const auto wrongRole = call(app, validBody, adminToken);
        tests.check(wrongRole.code == 401 || wrongRole.code == 403,
                    "an administrator token must not be accepted on a user route");
        const auto unknown = call(app, validBody, "not-a-real-token");
        tests.check(unknown.code == 401, "an unknown token must be rejected");
    }

    // —— 参数校验 ——
    {
        tests.check(call(app, R"({})", token).code == 422, "a missing message must be rejected");
        tests.check(call(app, R"({"message":""})", token).code == 422,
                    "an empty message must be rejected");
        tests.check(call(app, R"({"message":"   "})", token).code == 422,
                    "a whitespace-only message must be rejected");
        tests.check(
            call(app, std::string(R"({"message":")") + std::string(601, 'a') + "\"}", token).code ==
                422,
            "a message longer than 600 characters must be rejected");
        tests.check(call(app, R"({"message":"附近充电站","userId":9})", token).code == 422,
                    "unknown JSON fields must be rejected");
        tests.check(call(app, R"({"message":5})", token).code == 422,
                    "a non-string message must be rejected");
        tests.check(call(app, R"({"message":"附近充电站","location":"beijing"})", token).code ==
                        422,
                    "a non-object location must be rejected");
        tests.check(
            call(app, R"({"message":"附近充电站","location":{"latitudeE6":39977680}})", token)
                    .code == 422,
            "a location with only a latitude must be rejected");
        tests.check(
            call(
                app,
                R"({"message":"附近充电站","location":{"latitudeE6":99999999,"longitudeE6":116316417}})",
                token)
                    .code == 422,
            "an out-of-range latitude must be rejected");
        tests.check(call(app,
                         R"({"message":"附近充电站","location":{"latitudeE6":0,"longitudeE6":0}})",
                         token)
                            .code == 422,
                    "a null island location must be rejected");
        tests.check(
            call(
                app,
                R"({"message":"附近充电站","location":{"latitudeE6":39977680,"longitudeE6":116316417,"radius":5}})",
                token)
                    .code == 422,
            "unknown nested location fields must be rejected");
        tests.check(
            call(
                app,
                R"({"message":"附近充电站","location":{"latitudeE6":39977680,"longitudeE6":116316417},"coordinateType":"bd09"})",
                token)
                    .code == 422,
            "an unsupported coordinate type must be rejected");
        tests.check(
            call(
                app,
                R"({"message":"附近充电站","location":{"latitudeE6":39977680,"longitudeE6":116316417},"chargerType":5})",
                token)
                    .code == 422,
            "an unsupported charger type must be rejected");
        tests.check(!call(app, validBody, token).body.empty(),
                    "a valid request must produce a body");
        tests.check(
            call(app, R"({"message":"附近充电站"})", token).code == 200,
            "a request without a location must still be accepted (keyword/default fallback)");
        tests.check(
            call(
                app,
                R"({"message":"附近充电站","location":{"latitudeE6":39977680,"longitudeE6":116316417},"coordinateType":"wgs84","chargerType":1})",
                token)
                    .code == 200,
            "a fully specified request must be accepted");
    }

    // —— 结构化响应 ——
    {
        const auto response = call(app, validBody, token);
        tests.check(response.code == 200, "a valid agent request must return 200");
        const auto root = envelope(response);
        tests.check(root.value(QStringLiteral("success")).toBool() &&
                        root.value(QStringLiteral("code")).toInt(-1) == 0,
                    "the response must use the standard success envelope");
        const auto data = dataOf(response);
        tests.check(data.value(QStringLiteral("reply")).isString() &&
                        !data.value(QStringLiteral("reply")).toString().isEmpty(),
                    "the reply must never be empty");
        tests.check(data.value(QStringLiteral("stations")).isArray() &&
                        !data.value(QStringLiteral("stations")).toArray().isEmpty(),
                    "the response must carry structured stations, not only text");
        tests.check(data.value(QStringLiteral("pois")).isArray(),
                    "pois must always be present, empty when nothing was found");
        tests.check(data.value(QStringLiteral("route")).isNull(),
                    "route must be null when no route was requested");
        tests.check(data.value(QStringLiteral("actions")).isArray() &&
                        !data.value(QStringLiteral("actions")).toArray().isEmpty(),
                    "stations must produce frontend actions");
        tests.check(data.value(QStringLiteral("tools")).isArray() &&
                        !data.value(QStringLiteral("tools")).toArray().isEmpty(),
                    "executed tools must be reported for end-to-end assertions");
        tests.check(data.value(QStringLiteral("llmUsed")).isBool() &&
                        !data.value(QStringLiteral("llmUsed")).toBool(),
                    "an unconfigured LLM must be reported honestly");
        tests.check(data.value(QStringLiteral("degraded")).toBool(),
                    "an unconfigured LLM must mark the response as degraded");

        const auto station = data.value(QStringLiteral("stations")).toArray().first().toObject();
        for (const auto* field :
             {"id", "code", "name", "address", "adcode", "latitudeE6", "longitudeE6",
              "electricityPriceCentPerKwh", "servicePriceCentPerKwh", "totalPriceCentPerKwh",
              "idleCount", "operationalCount", "totalCount", "distanceMeter"})
        {
            tests.check(station.contains(QLatin1String(field)),
                        "the agent station DTO must stay identical to the station list DTO");
        }
        tests.check(station.value(QStringLiteral("totalPriceCentPerKwh")).isDouble(),
                    "prices must stay integer cents in the transport contract");
        tests.check(station.value(QStringLiteral("distanceMeter")).isDouble(),
                    "distance must stay integer metres in the transport contract");

        const auto action = data.value(QStringLiteral("actions")).toArray().first().toObject();
        tests.check(action.value(QStringLiteral("type")).toString() ==
                        QStringLiteral("open_station"),
                    "an open_station action must be offered for the nearest station");
        tests.check(action.contains(QStringLiteral("targetId")) &&
                        action.contains(QStringLiteral("url")),
                    "action DTO must expose a stable target and url field");
    }

    // —— 请求中的定位必须真正参与排序 ——
    {
        const auto response = call(
            app,
            R"({"message":"附近哪里有充电站","location":{"latitudeE6":39908372,"longitudeE6":116457658}})",
            token);
        const auto stations = dataOf(response).value(QStringLiteral("stations")).toArray();
        tests.check(stations.size() >= 1 &&
                        stations.first().toObject().value(QStringLiteral("code")).toString() ==
                            QStringLiteral("CBD"),
                    "the request location must drive the nearby ordering");
    }

    // —— POI 结果必须进入结构化响应 ——
    {
        const auto response = call(
            app,
            R"({"message":"充电站旁边哪里可以喝咖啡","location":{"latitudeE6":39977680,"longitudeE6":116316417}})",
            token);
        const auto data = dataOf(response);
        tests.check(poiProvider.calls == 1, "a POI question must trigger exactly one POI lookup");
        tests.check(data.value(QStringLiteral("pois")).toArray().size() == 1,
                    "POI items must be surfaced for the frontend cards");
        const auto poiJson = data.value(QStringLiteral("pois")).toArray().first().toObject();
        tests.check(poiJson.value(QStringLiteral("name")).toString() ==
                        QStringLiteral("星巴克(中关村店)"),
                    "the POI name must be transported verbatim");
        for (const auto* field : {"id", "name", "category", "address", "tel", "latitudeE6",
                                  "longitudeE6", "distanceMeter"})
        {
            tests.check(poiJson.contains(QLatin1String(field)),
                        "the POI DTO must expose a stable field set");
        }
    }

    // —— 外部地图 POI 失败：降级但仍返回站点 ——
    {
        const auto response = call(
            app,
            R"({"message":"充电站旁边哪里有商场","location":{"latitudeE6":39977680,"longitudeE6":116316417}})",
            token);
        const auto data = dataOf(response);
        tests.check(response.code == 200, "a failing POI provider must not fail the request");
        tests.check(data.value(QStringLiteral("pois")).toArray().isEmpty(),
                    "a failing POI provider must not fabricate POI results");
        tests.check(data.value(QStringLiteral("degraded")).toBool(),
                    "a failing POI provider must be reported as degraded");
        tests.check(!data.value(QStringLiteral("stations")).toArray().isEmpty(),
                    "station results must survive a POI failure");
    }

    // —— WGS84 起点转换失败：路线降级而不是编造起点 ——
    {
        planner.normalized = std::nullopt;
        const auto response = call(
            app,
            R"({"message":"导航到最近的充电站","location":{"latitudeE6":39977680,"longitudeE6":116316417},"coordinateType":"wgs84"})",
            token);
        const auto data = dataOf(response);
        tests.check(response.code == 200,
                    "a failed coordinate conversion must not fail the request");
        tests.check(planner.normalizeCalls >= 1, "the wgs84 flag must reach the map route service");
        tests.check(data.value(QStringLiteral("route")).isNull(),
                    "a failed conversion must not invent a route origin");
    }

    // —— 路线成功：结构化 route 与 navigate 动作 ——
    {
        planner.normalized = RoutePoint{39978100, 116316000};
        PlannedRoute planned;
        planned.distanceMeter = 4300;
        planned.durationSecond = 900;
        planned.polyline = {RoutePoint{39978100, 116316000}, RoutePoint{39983700, 116315200}};
        planned.steps = {{"向东行驶 300 米", 300, 60}};
        planner.next = planned;
        const auto response = call(
            app,
            R"({"message":"导航到最近的充电站要多久","location":{"latitudeE6":39977680,"longitudeE6":116316417},"coordinateType":"wgs84"})",
            token);
        const auto route = dataOf(response).value(QStringLiteral("route")).toObject();
        tests.check(!route.isEmpty(), "a successful route must be transported structurally");
        for (const auto* field :
             {"destinationName", "originLatitudeE6", "originLongitudeE6", "destinationLatitudeE6",
              "destinationLongitudeE6", "distanceMeter", "durationSecond", "provider", "fallback",
              "browserUrl", "steps", "polyline"})
        {
            tests.check(route.contains(QLatin1String(field)),
                        "the route DTO must expose a stable field set");
        }
        tests.check(route.value(QStringLiteral("provider")).toString() ==
                            QStringLiteral("TENCENT_MAP") &&
                        !route.value(QStringLiteral("fallback")).toBool(),
                    "a usable Tencent route must not be marked as a fallback");
        tests.check(route.value(QStringLiteral("originLatitudeE6")).toInteger(0) == 39978100,
                    "the converted WGS84 origin must be used in the response");
        tests.check(route.value(QStringLiteral("browserUrl"))
                        .toString()
                        .startsWith(QStringLiteral("https://apis.map.qq.com/uri/v1/routeplan")),
                    "the route must expose a browser navigation entry point");
        const auto actions = dataOf(response).value(QStringLiteral("actions")).toArray();
        const auto navigate =
            std::find_if(actions.begin(), actions.end(),
                         [](const QJsonValue& value)
                         {
                             return value.toObject().value(QStringLiteral("type")).toString() ==
                                    QStringLiteral("navigate");
                         });
        tests.check(navigate != actions.end(),
                    "a computed route must offer a navigate action for the frontend");
    }

    // —— 响应不得泄漏内部信息 ——
    {
        const auto body = call(app, validBody, token).body;
        for (const auto* forbidden : {"sqlite", "SELECT ", "AI_API_KEY", "/Users/", "apiKey"})
        {
            tests.check(body.find(forbidden) == std::string::npos,
                        "the agent response must not leak internal details");
        }
    }

    if (tests.result() == 0)
        std::cout << "agent controller tests passed\n";
    return tests.result();
}
