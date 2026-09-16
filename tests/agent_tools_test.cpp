// Agent 工具单元测试：四个第一阶段工具的直接调用契约。
// 覆盖 station_search 的距离/价格/空闲/类型筛选、station_detail 的详情与充电桩、poi_search 的
// 无服务/无圆心/空结果/成功、route 的缺目的地、WGS84 转换失败、成功路线与 Haversine 降级。
// 全部使用内存仓储与测试桩；不访问 SQLite、网络或真实腾讯地图。
#include "agent/tools/poi_search_tool.h"
#include "agent/tools/route_tool.h"
#include "agent/tools/station_detail_tool.h"
#include "agent/tools/station_search_tool.h"

#include "core/application/charging_repository.h"
#include "core/application/poi_service.h"
#include "core/application/station_service.h"
#include "infrastructure/map/route_service.h"

#include <QJsonObject>

#include <algorithm>
#include <chrono>
#include <iostream>
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

class FixedPlanner final : public RoutePlanner
{
  public:
    std::optional<PlannedRoute> next;
    std::optional<RoutePoint> normalized;
    int planCalls = 0;

    std::optional<RoutePoint> normalizeGps(RoutePoint) override
    {
        return normalized;
    }

    std::optional<PlannedRoute> plan(RoutePoint, RoutePoint, TravelMode) override
    {
        ++planCalls;
        return next;
    }
};

class FakePoiProvider final : public PoiProvider
{
  public:
    bool ready = true;
    ServiceResult<std::vector<PoiItem>> next;
    PoiQuery lastQuery;
    int calls = 0;

    bool available() const override
    {
        return ready;
    }

    ServiceResult<std::vector<PoiItem>> search(const PoiQuery& query) override
    {
        ++calls;
        lastQuery = query;
        if (!ready)
            return {ErrorCode::ExternalServiceUnavailable, std::nullopt};
        return next;
    }
};

PlannedRoute usableRoute()
{
    PlannedRoute planned;
    planned.distanceMeter = 4300;
    planned.durationSecond = 900;
    planned.polyline = {RoutePoint{39977680, 116316417}, RoutePoint{39983700, 116315200}};
    planned.steps = {{"向东行驶 300 米", 300, 60}};
    return planned;
}

ncs::agent::AgentContext locatedContext()
{
    ncs::agent::AgentContext context;
    context.userId = 11;
    context.latitudeE6 = 39977680;
    context.longitudeE6 = 116316417;
    context.now = std::chrono::system_clock::time_point(std::chrono::seconds(1788500000));
    return context;
}

} // namespace

int main()
{
    using namespace ncs;
    TestRunner tests;

    core::application::InMemoryChargingRepository repository;
    FixedGeocoder geocoder;
    core::application::StationService stations(repository, geocoder);
    const auto context = locatedContext();

    // ================= station_search =================
    {
        agent::StationSearchTool tool(stations);
        tests.check(tool.name() == "station_search", "tool name must stay stable");
        tests.check(!tool.parametersSchema().empty(), "the tool must publish a parameter schema");

        const auto all = tool.invoke(context, QJsonObject{});
        tests.check(all.ok && all.stations.size() == 3,
                    "without filters every demo station must be returned");
        tests.check(!all.observation.empty() && all.observation.front() != '{',
                    "the observation handed to the model must be readable text");
        tests.check(!all.payload.isEmpty(), "structured extra payload must be reported");

        // 距离筛选
        const auto near =
            tool.invoke(context, QJsonObject{{QStringLiteral("maxDistanceMeter"), 3000}});
        tests.check(near.ok && near.stations.size() == 1 && near.stations.front().code == "ZGC",
                    "maxDistanceMeter must filter by Haversine distance from the user");

        // 空闲数量筛选：中关村有 5 个空闲桩，西二旗有 2 个（1 快 1 慢），国贸 1 快充桩故障
        const auto idle = tool.invoke(context, QJsonObject{{QStringLiteral("minIdleCount"), 2}});
        tests.check(idle.ok && !idle.stations.empty() &&
                        std::all_of(idle.stations.begin(), idle.stations.end(),
                                    [](const core::application::StationSummary& station)
                                    { return station.idleCount >= 2; }),
                    "minIdleCount must drop stations without enough idle chargers");

        // 价格筛选：国贸 92+48=140 分，中关村 85+50=135 分
        const auto cheap =
            tool.invoke(context, QJsonObject{{QStringLiteral("maxTotalPriceCentPerKwh"), 136}});
        tests.check(cheap.ok && !cheap.stations.empty() &&
                        std::all_of(cheap.stations.begin(), cheap.stations.end(),
                                    [](const core::application::StationSummary& station)
                                    { return station.totalPriceCentPerKwh <= 136; }),
                    "maxTotalPriceCentPerKwh must drop stations above the price ceiling");

        // 条数上限
        const auto limited = tool.invoke(context, QJsonObject{{QStringLiteral("limit"), 1}});
        tests.check(limited.ok && limited.stations.size() == 1,
                    "limit must cap the number of returned stations");

        // 会话上下文里的充电类型偏好必须生效
        agent::AgentContext fastOnly = context;
        fastOnly.chargerType = 1;
        const auto fast = tool.invoke(fastOnly, QJsonObject{});
        tests.check(fast.ok && !fast.stations.empty(),
                    "a charger-type preference must not empty the result set");

        // 只有一半坐标：视为无效，回落会话定位而不是抛错
        const auto halfPair =
            tool.invoke(context, QJsonObject{{QStringLiteral("latitudeE6"), 31000000}});
        tests.check(halfPair.ok && !halfPair.stations.empty(),
                    "a half-specified coordinate pair must fall back to the session location");

        // 越界坐标必须被拒绝并回落到会话定位
        const auto outOfRange =
            tool.invoke(context, QJsonObject{{QStringLiteral("latitudeE6"), 999999999}});
        tests.check(outOfRange.ok && !outOfRange.stations.empty(),
                    "an out-of-range coordinate must be discarded, not thrown");

        // 无定位且无关键词：必须降级到默认坐标而不是失败
        agent::AgentContext blind;
        blind.now = context.now;
        const auto fallback = tool.invoke(blind, QJsonObject{});
        tests.check(
            fallback.ok && fallback.stations.size() == 3 &&
                fallback.payload.value(QStringLiteral("locationFallback")).toBool(),
            "a missing location must degrade to preset coordinates and report the fallback");

        // 过滤后为空仍是成功查询（没有任何站点有 100 个空闲桩）
        const auto none = tool.invoke(context, QJsonObject{{QStringLiteral("minIdleCount"), 100}});
        tests.check(none.ok && none.stations.empty() &&
                        none.observation.find("没有符合") != std::string::npos,
                    "an empty filtered result must stay a successful query with an explicit note");
    }

    // ================= station_detail =================
    {
        agent::StationDetailTool tool(stations);
        tests.check(tool.name() == "station_detail", "tool name must stay stable");

        const auto missing = tool.invoke(context, QJsonObject{});
        tests.check(!missing.ok, "station_detail without a station id must fail explicitly");
        tests.check(!missing.observation.empty(),
                    "a failed tool must still produce a model-readable observation");

        const auto unknown = tool.invoke(context, QJsonObject{{QStringLiteral("stationId"), 999}});
        tests.check(!unknown.ok, "an unknown station id must fail instead of inventing a station");

        const auto found = tool.invoke(context, QJsonObject{{QStringLiteral("stationId"), 1}});
        tests.check(found.ok && found.stations.size() == 1 && found.stations.front().code == "ZGC",
                    "an existing station must be returned in full");
        tests.check(found.observation.find("营业时间") != std::string::npos &&
                        found.observation.find("充电桩明细") != std::string::npos,
                    "the detail observation must include opening hours and charger details");
        tests.check(found.payload.value(QStringLiteral("chargerTotal")).toInteger(0) == 5,
                    "the charger page size must be reported in the structured payload");

        // 充电类型过滤后仍应成功
        const auto fast = tool.invoke(context, QJsonObject{{QStringLiteral("stationId"), 1},
                                                           {QStringLiteral("chargerType"), 1}});
        tests.check(fast.ok, "a charger-type filter must not break station detail");
    }

    // ================= poi_search =================
    {
        FakePoiProvider provider;
        agent::PoiSearchTool tool(provider);
        tests.check(tool.name() == "poi_search", "tool name must stay stable");

        provider.ready = false;
        const auto unconfigured = tool.invoke(context, QJsonObject{});
        tests.check(!unconfigured.ok &&
                        unconfigured.observation.find("未配置") != std::string::npos,
                    "an unconfigured map service must degrade with an explicit note");

        provider.ready = true;
        PoiItem poi;
        poi.id = "POI-1";
        poi.name = "星巴克(中关村店)";
        poi.category = "咖啡厅";
        poi.address = "北京市海淀区中关村大街 1 号";
        poi.latitudeE6 = 39978100;
        poi.longitudeE6 = 116316000;
        poi.distanceMeter = 210;
        provider.next = {ErrorCode::Ok, std::vector<PoiItem>{poi}};

        const auto found = tool.invoke(context, QJsonObject{{QStringLiteral("category"), "咖啡"}});
        tests.check(found.ok && found.pois.size() == 1 &&
                        found.pois.front().name == "星巴克(中关村店)",
                    "a successful POI lookup must surface the items");
        tests.check(provider.lastQuery.category == "咖啡" &&
                        provider.lastQuery.latitudeE6 == context.latitudeE6 &&
                        provider.lastQuery.longitudeE6 == context.longitudeE6,
                    "the query must carry the category and the session centre");
        tests.check(found.observation.find("星巴克") != std::string::npos,
                    "the observation must name the POIs for the model");

        // 显式圆心优先于会话定位
        const auto explicitCentre =
            tool.invoke(context, QJsonObject{{QStringLiteral("latitudeE6"), 40000000},
                                             {QStringLiteral("longitudeE6"), 116400000}});
        tests.check(explicitCentre.ok && provider.lastQuery.latitudeE6 == 40000000,
                    "an explicit centre must override the session location");

        // 越界的半径与条数必须被丢弃并回落到默认值，而不是失败或透传
        const auto outOfRange =
            tool.invoke(context, QJsonObject{{QStringLiteral("radiusMeter"), 999999},
                                             {QStringLiteral("limit"), 999}});
        tests.check(outOfRange.ok && provider.lastQuery.radiusMeter == 2000 &&
                        provider.lastQuery.limit == 5,
                    "out-of-range radius and limit must fall back to the documented defaults");

        // 空结果仍是成功查询
        provider.next = {ErrorCode::Ok, std::vector<PoiItem>{}};
        const auto empty = tool.invoke(context, QJsonObject{});
        tests.check(empty.ok && empty.pois.empty() &&
                        empty.observation.find("没有匹配") != std::string::npos,
                    "an empty POI result must be a successful query with an explicit note");

        // 无圆心且无定位：明确失败
        agent::AgentContext blind;
        blind.now = context.now;
        const auto noCentre = tool.invoke(blind, QJsonObject{});
        tests.check(!noCentre.ok && noCentre.observation.find("圆心") != std::string::npos,
                    "POI search without a centre must fail explicitly");

        // 地图返回参数非法：映射为可读降级说明
        provider.next = {ErrorCode::ValidationFailed, std::nullopt};
        const auto invalid = tool.invoke(context, QJsonObject{});
        tests.check(!invalid.ok, "an invalid centre reported by the provider must fail the tool");
    }

    // ================= route =================
    {
        FixedPlanner planner;
        infrastructure::map::RouteService routes(planner);
        agent::RouteTool tool(routes);
        tests.check(tool.name() == "route", "tool name must stay stable");

        const auto noDestination = tool.invoke(context, QJsonObject{});
        tests.check(!noDestination.ok &&
                        noDestination.observation.find("目的地") != std::string::npos,
                    "route without a destination must fail explicitly");

        agent::AgentContext blind;
        blind.now = context.now;
        const auto noOrigin =
            tool.invoke(blind, QJsonObject{{QStringLiteral("destinationLatitudeE6"), 39983700},
                                           {QStringLiteral("destinationLongitudeE6"), 116315200}});
        tests.check(!noOrigin.ok && noOrigin.observation.find("起点") != std::string::npos,
                    "route without any origin must fail explicitly");

        // WGS84 起点转换失败：必须失败，不能用默认坐标掩盖
        planner.normalized = std::nullopt;
        agent::AgentContext gps = context;
        gps.wgs84Location = true;
        const auto conversionFailed =
            tool.invoke(gps, QJsonObject{{QStringLiteral("destinationLatitudeE6"), 39983700},
                                         {QStringLiteral("destinationLongitudeE6"), 116315200}});
        tests.check(!conversionFailed.ok &&
                        conversionFailed.observation.find("重新定位") != std::string::npos,
                    "a failed WGS84 conversion must fail instead of inventing an origin");

        // WGS84 起点转换成功：使用转换后的坐标
        planner.normalized = RoutePoint{39978100, 116316000};
        planner.next = usableRoute();
        const auto gpsRoute =
            tool.invoke(gps, QJsonObject{{QStringLiteral("destinationLatitudeE6"), 39983700},
                                         {QStringLiteral("destinationLongitudeE6"), 116315200},
                                         {QStringLiteral("destinationName"), "NCS 中关村充电站"}});
        tests.check(gpsRoute.ok && gpsRoute.route.has_value() &&
                        gpsRoute.route->originLatitudeE6 == 39978100,
                    "a converted WGS84 origin must be used for planning");
        tests.check(gpsRoute.route.has_value() && gpsRoute.route->distanceMeter == 4300 &&
                        gpsRoute.route->durationSecond == 900 &&
                        gpsRoute.route->provider == "TENCENT_MAP" && !gpsRoute.route->fallback,
                    "a successful map route must be reported as a Tencent route");
        tests.check(gpsRoute.route.has_value() && gpsRoute.route->steps.size() == 1 &&
                        gpsRoute.route->polyline.size() == 2,
                    "steps and polyline must be carried through for map rendering");
        tests.check(gpsRoute.route.has_value() &&
                        gpsRoute.route->browserUrl.rfind("https://apis.map.qq.com/uri/v1/routeplan",
                                                         0) == 0,
                    "the route must expose a browser navigation entry point");
        tests.check(gpsRoute.observation.find("预计") != std::string::npos,
                    "the observation must include the estimated travel information");

        // 地图规划失败：降级为 Haversine 直线，仍然成功返回
        FixedPlanner failing;
        failing.next = std::nullopt;
        infrastructure::map::RouteService fallbackRoutes(failing);
        agent::RouteTool fallbackTool(fallbackRoutes);
        const auto degraded = fallbackTool.invoke(
            context, QJsonObject{{QStringLiteral("destinationLatitudeE6"), 39983700},
                                 {QStringLiteral("destinationLongitudeE6"), 116315200}});
        tests.check(degraded.ok && degraded.route.has_value() && degraded.route->fallback &&
                        degraded.route->provider == "LOCAL_FALLBACK" &&
                        degraded.route->durationSecond == 0,
                    "an unavailable planner must degrade to a straight-line estimate");
        tests.check(degraded.ok && degraded.route.has_value() &&
                        degraded.route->distanceMeter > 0 && degraded.route->polyline.size() == 2,
                    "a degraded route must still report a real distance and the two endpoints");
        tests.check(degraded.observation.find("直线距离") != std::string::npos,
                    "the degraded route must be labelled for the model");

        // 起终点重合：不调用第三方，距离为 0
        FixedPlanner same;
        same.next = usableRoute();
        infrastructure::map::RouteService sameRoutes(same);
        agent::RouteTool sameTool(sameRoutes);
        const auto coincident = sameTool.invoke(
            context, QJsonObject{{QStringLiteral("destinationLatitudeE6"), *context.latitudeE6},
                                 {QStringLiteral("destinationLongitudeE6"), *context.longitudeE6}});
        tests.check(coincident.ok && same.planCalls == 0,
                    "coincident origin and destination must not call the external planner");

        // 出行方式透传
        FixedPlanner walking;
        walking.next = usableRoute();
        infrastructure::map::RouteService walkingRoutes(walking);
        agent::RouteTool walkingTool(walkingRoutes);
        const auto walk = walkingTool.invoke(
            context, QJsonObject{{QStringLiteral("destinationLatitudeE6"), 39983700},
                                 {QStringLiteral("destinationLongitudeE6"), 116315200},
                                 {QStringLiteral("mode"), "walking"}});
        tests.check(walk.ok && walk.route.has_value() &&
                        walk.route->browserUrl.find("type=walk") != std::string::npos,
                    "the requested travel mode must reach the navigation link");
    }

    if (tests.result() == 0)
        std::cout << "agent tools tests passed\n";
    return tests.result();
}
