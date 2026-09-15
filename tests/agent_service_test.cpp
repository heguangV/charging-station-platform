// AgentService 编排测试：工具注册与确定性意图判断、工具调度与锚点注入、结构化结果汇总、
// LLM 未配置 / 调用失败 / 超时 / 返回空内容的降级路径、无定位与无结果、工具失败隔离与调用上限。
// 全部使用内存仓储与测试桩，不访问网络、SQLite 或真实腾讯地图。
#include "agent/agent_service.h"

#include "core/application/charging_repository.h"
#include "core/application/llm_client.h"
#include "core/application/poi_service.h"
#include "core/application/station_service.h"

#include <QByteArray>
#include <QJsonDocument>
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
using ncs::core::application::LlmChatRequest;
using ncs::core::application::LlmChatResponse;
using ncs::core::application::LlmClient;
using ncs::core::application::LlmMessage;
using ncs::core::application::LlmRole;
using ncs::core::application::LlmToolCall;
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

// 可编程 LLM 桩：记录每次请求，按调用序号返回预设结果，用来覆盖“意图选择—工具—汇总”整条链路。
class ScriptedLlm final : public LlmClient
{
  public:
    bool available() const override
    {
        return enabled;
    }

    ServiceResult<LlmChatResponse> chat(const LlmChatRequest& request) override
    {
        requests.push_back(request);
        if (responses.size() <= static_cast<std::size_t>(callIndex))
            return {ErrorCode::ExternalServiceUnavailable, std::nullopt};
        return responses[static_cast<std::size_t>(callIndex++)];
    }

    bool enabled = true;
    std::size_t callIndex = 0;
    std::vector<LlmChatRequest> requests;
    std::vector<ServiceResult<LlmChatResponse>> responses;
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

LlmChatResponse toolCallResponse(const std::string& name, const std::string& arguments)
{
    LlmChatResponse response;
    LlmToolCall call;
    call.id = "call_1";
    call.name = name;
    call.argumentsJson = arguments;
    response.toolCalls.push_back(call);
    response.finishReason = "tool_calls";
    return response;
}

ServiceResult<LlmChatResponse> okReply(LlmChatResponse response)
{
    return {ErrorCode::Ok, std::move(response)};
}

ServiceResult<LlmChatResponse> failedReply(const ErrorCode code)
{
    return {code, std::nullopt};
}

LlmChatResponse contentResponse(const std::string& content)
{
    LlmChatResponse response;
    response.content = content;
    response.finishReason = "stop";
    return response;
}

std::vector<PoiItem> samplePois()
{
    PoiItem poi;
    poi.id = "POI-1";
    poi.name = "星巴克(中关村店)";
    poi.category = "咖啡厅";
    poi.address = "北京市海淀区中关村大街 1 号";
    poi.latitudeE6 = 39978100;
    poi.longitudeE6 = 116316000;
    poi.distanceMeter = 210;
    return {poi};
}

ncs::agent::AgentContext locatedContext()
{
    ncs::agent::AgentContext context;
    context.userId = 7;
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

    // —— 工具注册 ——
    {
        FixedPlanner planner;
        FakePoiProvider pois;
        ScriptedLlm llm;
        agent::AgentService service(stations, planner, pois, llm);
        const auto names = service.toolNames();
        tests.check(names.size() == 4, "exactly four tools must be registered in phase one");
        tests.check(names.size() == 4 && names[0] == "station_search" &&
                        names[1] == "station_detail" && names[2] == "poi_search" &&
                        names[3] == "route",
                    "tool registration order must be stable and observable");
        tests.check(service.tool("station_search") != nullptr &&
                        service.tool("poi_search") != nullptr && service.tool("route") != nullptr &&
                        service.tool("station_detail") != nullptr,
                    "every phase-one tool must be resolvable by name");
        tests.check(service.tool("order_refund") == nullptr,
                    "unregistered tool names must not resolve");

        for (const auto name : {"station_search", "station_detail", "poi_search", "route"})
        {
            auto* tool = service.tool(name);
            tests.check(tool != nullptr && !tool->description().empty(),
                        "every tool must describe itself for the model");
            tests.check(
                tool != nullptr &&
                    QJsonDocument::fromJson(
                        QByteArray::fromRawData(tool->parametersSchema().data(),
                                                static_cast<int>(tool->parametersSchema().size())))
                        .isObject(),
                "every tool must expose a parseable JSON Schema object");
        }
    }

    // —— 确定性意图判断：LLM 不可用时的降级计划 ——
    {
        FixedPlanner planner;
        FakePoiProvider pois;
        ScriptedLlm llm;
        agent::AgentService service(stations, planner, pois, llm);

        const auto nearby = service.planTools("附近哪里有充电站");
        tests.check(nearby.size() == 1 && nearby.front() == "station_search",
                    "a plain nearby question plans station_search only");

        const auto dining = service.planTools("帮我找一个附近有快充并且旁边能吃饭的充电站");
        tests.check(std::find(dining.begin(), dining.end(), "station_search") != dining.end() &&
                        std::find(dining.begin(), dining.end(), "poi_search") != dining.end(),
                    "a dining question must plan both station_search and poi_search");

        const auto navigation = service.planTools("导航到最近的充电站要多久");
        tests.check(std::find(navigation.begin(), navigation.end(), "route") != navigation.end(),
                    "a navigation question must plan route");

        const auto detail = service.planTools("这个站有多少充电桩，几点营业");
        tests.check(std::find(detail.begin(), detail.end(), "station_detail") != detail.end(),
                    "a device/opening-hours question must plan station_detail");

        const auto unknown = service.planTools("你好");
        tests.check(unknown.size() == 1 && unknown.front() == "station_search",
                    "an unrecognised intent still plans the station search anchor");
    }

    // —— LLM 不可用：确定性兜底必须给出站点数据与说明文字，且标记降级 ——
    {
        FixedPlanner planner;
        FakePoiProvider pois;
        ScriptedLlm llm;
        llm.enabled = false;
        agent::AgentService service(stations, planner, pois, llm);
        const auto result = service.chat("附近哪里有充电站", locatedContext());

        tests.check(!result.llmUsed, "an unconfigured LLM must not be reported as used");
        tests.check(result.degraded, "an unconfigured LLM must mark the answer as degraded");
        tests.check(!result.reply.empty(), "a fallback reply must never be empty");
        tests.check(!result.stations.empty(),
                    "the fallback path must still return structured stations");
        tests.check(result.tools.size() == 1 && result.tools.front() == "station_search",
                    "only the planned tool may be executed");
        tests.check(llm.requests.empty(),
                    "no LLM request may be issued when the client is unavailable");
        tests.check(!result.actions.empty() && result.actions.front().type == "open_station",
                    "stations must produce open_station actions for the frontend");
    }

    // —— LLM 调用失败：降级但不失败 ——
    {
        FixedPlanner planner;
        FakePoiProvider pois;
        ScriptedLlm llm;
        llm.responses.push_back(failedReply(ErrorCode::ExternalServiceUnavailable));
        agent::AgentService service(stations, planner, pois, llm);
        const auto result = service.chat("附近哪里有充电站", locatedContext());

        tests.check(!result.llmUsed && result.degraded, "a failing LLM must degrade the answer");
        tests.check(!result.reply.empty() && !result.stations.empty(),
                    "a failing LLM must still return data and text");
    }

    // —— LLM 超时（表现为未返回结果）与返回空内容 ——
    {
        FixedPlanner planner;
        FakePoiProvider pois;
        ScriptedLlm llm;
        llm.responses.push_back(failedReply(ErrorCode::ExternalServiceUnavailable));
        llm.responses.push_back(failedReply(ErrorCode::ExternalServiceUnavailable));
        agent::AgentService service(stations, planner, pois, llm);
        const auto timeout = service.chat("附近哪里有充电站", locatedContext());
        tests.check(timeout.degraded && !timeout.reply.empty(),
                    "an LLM timeout must degrade to a deterministic reply");

        ScriptedLlm empty;
        empty.responses.push_back(okReply(toolCallResponse("station_search", "{}")));
        empty.responses.push_back(okReply(contentResponse("   ")));
        agent::AgentService emptyService(stations, planner, pois, empty);
        const auto blank = emptyService.chat("附近哪里有充电站", locatedContext());
        tests.check(!blank.llmUsed && blank.degraded,
                    "blank LLM content must fall back to the deterministic reply");
        tests.check(blank.reply.find("NCS") != std::string::npos ||
                        blank.reply.find("充电站") != std::string::npos,
                    "the deterministic reply must still describe the found stations");
    }

    // —— 模型返回未注册工具名：忽略并补齐确定性计划 ——
    {
        FixedPlanner planner;
        FakePoiProvider pois;
        ScriptedLlm llm;
        llm.responses.push_back(okReply(toolCallResponse("issue_refund", "{}")));
        llm.responses.push_back(okReply(contentResponse("已为你查询到附近充电站。")));
        agent::AgentService service(stations, planner, pois, llm);
        const auto result = service.chat("附近哪里有充电站", locatedContext());
        tests.check(
            result.tools.size() == 1 && result.tools.front() == "station_search",
            "an unregistered model tool name must be ignored and replaced by the safe plan");
        tests.check(result.llmUsed && result.reply == "已为你查询到附近充电站。",
                    "the second round must reuse the model reply when it succeeds");
    }

    // —— 模型选择 poi_search：锚点站点坐标必须被注入 ——
    {
        FixedPlanner planner;
        FakePoiProvider pois;
        pois.next = {ErrorCode::Ok, samplePois()};
        ScriptedLlm llm;
        LlmChatResponse plan = toolCallResponse("station_search", R"({"limit":2})");
        LlmToolCall alsoCoffee;
        alsoCoffee.id = "call_2";
        alsoCoffee.name = "poi_search";
        alsoCoffee.argumentsJson = R"({"category":"咖啡"})";
        plan.toolCalls.push_back(alsoCoffee);
        llm.responses.push_back(okReply(plan));
        llm.responses.push_back(okReply(contentResponse("推荐中关村充电站，旁边有咖啡店。")));
        agent::AgentService service(stations, planner, pois, llm);

        const auto result = service.chat("附近有快充并且旁边能喝咖啡的充电站", locatedContext());
        tests.check(result.tools.size() == 2 && result.tools[0] == "station_search" &&
                        result.tools[1] == "poi_search",
                    "station_search must execute before poi_search");
        tests.check(!result.stations.empty() && pois.calls == 1 &&
                        pois.lastQuery.latitudeE6 == result.stations.front().latitudeE6 &&
                        pois.lastQuery.longitudeE6 == result.stations.front().longitudeE6,
                    "poi_search must be anchored on the nearest station found first");
        tests.check(pois.lastQuery.category == "咖啡",
                    "the model-provided POI category must reach the provider");
        tests.check(result.pois.size() == 1 && result.pois.front().name == "星巴克(中关村店)",
                    "POI results must be surfaced in the structured response");
        tests.check(!result.route.has_value(),
                    "no route may be invented when route was not planned");
    }

    // —— 锚点站点注入 route 目的地 ——
    {
        FixedPlanner planner;
        FixedPlanner& mockPlanner = planner;
        PlannedRoute planned;
        planned.distanceMeter = 4300;
        planned.durationSecond = 900;
        planned.polyline = {RoutePoint{39977680, 116316417}, RoutePoint{39983700, 116315200}};
        planned.steps = {{"向东行驶 300 米", 300, 60}};
        mockPlanner.next = planned;
        FakePoiProvider pois;
        ScriptedLlm llm;
        llm.responses.push_back(okReply(toolCallResponse("route", "{}")));
        llm.responses.push_back(okReply(contentResponse("驾车约 15 分钟。")));
        agent::AgentService service(stations, mockPlanner, pois, llm);
        // 模型只选择了 route：AgentService 必须自动补一次站点检索以获得目的地锚点。
        // 起点刻意远离所有演示站点，避免起终点重合而绕过外部路线规划。
        agent::AgentContext away = locatedContext();
        away.latitudeE6 = 39900000;
        away.longitudeE6 = 116450000;
        const auto result = service.chat("导航到最近的充电站要多久", away);

        tests.check(result.tools.size() == 2 && result.tools[0] == "station_search" &&
                        result.tools[1] == "route",
                    "a route-only plan must be completed with a station anchor first");
        tests.check(result.route.has_value() && result.route->distanceMeter == 4300 &&
                        result.route->durationSecond == 900,
                    "a successful map route must be surfaced unchanged");
        tests.check(
            !result.route->browserUrl.empty() &&
                result.route->browserUrl.rfind("https://apis.map.qq.com/uri/v1/routeplan", 0) == 0,
            "the route must carry a browser navigation entry point");
        const auto navigate = std::find_if(result.actions.begin(), result.actions.end(),
                                           [](const agent::AgentAction& action)
                                           { return action.type == "navigate"; });
        tests.check(navigate != result.actions.end() && !navigate->url.empty(),
                    "a route must produce a navigate action");
    }

    // —— 地图路线失败：以直线距离降级但仍返回结果 ——
    {
        FixedPlanner planner;
        FakePoiProvider pois;
        ScriptedLlm llm;
        llm.responses.push_back(okReply(toolCallResponse("route", "{}")));
        llm.responses.push_back(okReply(contentResponse("驾车前往，距离为直线估算。")));
        agent::AgentService service(stations, planner, pois, llm);
        agent::AgentContext away = locatedContext();
        away.latitudeE6 = 39900000;
        away.longitudeE6 = 116450000;
        const auto result = service.chat("导航到最近的充电站", away);

        tests.check(result.route.has_value() && result.route->fallback,
                    "an unavailable map planner must degrade to a straight-line estimate");
        tests.check(result.route.has_value() && result.route->provider == "LOCAL_FALLBACK",
                    "the degraded route must be labelled as a local fallback");
        tests.check(result.route.has_value() && result.route->durationSecond == 0,
                    "a degraded route must not invent a travel duration");
    }

    // —— 工具失败必须被隔离：POI 不可用不影响站点结果 ——
    {
        FixedPlanner planner;
        FakePoiProvider pois;
        pois.ready = false;
        ScriptedLlm llm;
        llm.responses.push_back(okReply(toolCallResponse("station_search", "{}")));
        llm.responses.push_back(okReply(contentResponse("已找到附近充电站，附近暂无餐饮推荐。")));
        agent::AgentService service(stations, planner, pois, llm);
        const auto result = service.chat("附近有快充并且旁边能吃饭的充电站", locatedContext());

        tests.check(!result.stations.empty(), "a failing POI tool must not lose station results");
        tests.check(result.pois.empty(), "a failing POI tool must not fabricate POI results");
    }

    // —— 无定位：不得因此失败，仍要给出降级说明或默认位置结果 ——
    {
        FixedPlanner planner;
        FakePoiProvider pois;
        ScriptedLlm llm;
        llm.enabled = false;
        agent::AgentService service(stations, planner, pois, llm);
        agent::AgentContext blind;
        blind.userId = 7;
        blind.now = std::chrono::system_clock::time_point(std::chrono::seconds(1788500000));
        const auto result = service.chat("帮我找附近的充电站", blind);
        tests.check(!result.reply.empty(), "a missing location must still produce a reply");
        tests.check(
            !result.stations.empty(),
            "a missing location must degrade to preset coordinates, not to an empty answer");
    }

    // —— 无结果：查询成功但为空时给出明确文案且不编造 ——
    {
        FixedPlanner planner;
        FakePoiProvider pois;
        pois.next = {ErrorCode::Ok, std::vector<PoiItem>{}};
        ScriptedLlm llm;
        llm.enabled = false;
        agent::AgentService service(stations, planner, pois, llm);
        const auto result = service.chat("附近有能充电的地方和咖啡店吗", locatedContext());
        tests.check(result.pois.empty() && pois.calls == 1,
                    "an empty POI result must be surfaced as empty, not fabricated");
        tests.check(result.reply.find("暂时没有") != std::string::npos ||
                        result.reply.find("没有符合") != std::string::npos,
                    "an empty POI result must be reported honestly in the reply");
        tests.check(!result.stations.empty(),
                    "stations remain available when only the POI lookup is empty");
    }

    // —— 站点查询本身为空：确定性文案必须如实说明，不得编造站点 ——
    {
        FixedPlanner planner;
        FakePoiProvider pois;
        ScriptedLlm llm;
        // 模型请求一个极小的距离上限，内存仓库中的站点全部被过滤掉。
        llm.responses.push_back(
            okReply(toolCallResponse("station_search", R"({"minIdleCount":100})")));
        llm.responses.push_back(failedReply(ErrorCode::InternalError));
        agent::AgentService service(stations, planner, pois, llm);
        const auto result = service.chat("附近哪里有充电站", locatedContext());
        tests.check(result.stations.empty(),
                    "a fully filtered station query must return no stations");
        tests.check(result.reply.find("暂时没有符合条件的充电站") != std::string::npos,
                    "an empty station query must be reported honestly");
    }

    // —— 工具调用上限：模型重复调用同名工具只执行一次 ——
    {
        FixedPlanner planner;
        FakePoiProvider pois;
        ScriptedLlm llm;
        LlmChatResponse many;
        for (int index = 0; index < 3; ++index)
        {
            LlmToolCall call;
            call.id = "call_" + std::to_string(index);
            call.name = "station_search";
            call.argumentsJson = "{}";
            many.toolCalls.push_back(call);
        }
        llm.responses.push_back(okReply(many));
        llm.responses.push_back(okReply(contentResponse("已找到附近充电站。")));
        agent::AgentService service(stations, planner, pois, llm);
        const auto result = service.chat("附近哪里有充电站", locatedContext());
        tests.check(result.tools.size() == 1,
                    "duplicate tool calls in one response must be executed once");
    }

    // —— 工具参数越界：不可信参数被丢弃而非抛异常 ——
    {
        FixedPlanner planner;
        FakePoiProvider pois;
        ScriptedLlm llm;
        llm.responses.push_back(okReply(
            toolCallResponse("station_search", R"({"latitudeE6": 999999999, "limit": 500})")));
        llm.responses.push_back(okReply(contentResponse("已按当前位置查询。")));
        agent::AgentService service(stations, planner, pois, llm);
        const auto result = service.chat("附近哪里有充电站", locatedContext());
        tests.check(!result.stations.empty(),
                    "out-of-range model arguments must fall back to the session context");
    }

    // —— LLM 提示词与工具声明必须真的发给模型 ——
    {
        FixedPlanner planner;
        FakePoiProvider pois;
        ScriptedLlm llm;
        llm.responses.push_back(okReply(toolCallResponse("station_search", "{}")));
        llm.responses.push_back(okReply(contentResponse("已找到。")));
        agent::AgentService service(stations, planner, pois, llm);
        service.chat("附近哪里有充电站", locatedContext());
        tests.check(llm.requests.size() == 2,
                    "a successful tool round must issue exactly two calls");
        tests.check(!llm.requests.empty() && !llm.requests[0].tools.empty() &&
                        llm.requests[0].tools.size() == 4,
                    "the first call must advertise every registered tool");
        tests.check(!llm.requests.empty() && llm.requests[0].messages.size() == 2 &&
                        llm.requests[0].messages.front().role == LlmRole::System &&
                        llm.requests[0].messages[1].role == LlmRole::User,
                    "the first call must send a system prompt and the user question");
        tests.check(llm.requests.size() == 2 && llm.requests[1].messages.size() == 3 &&
                        llm.requests[1].messages.back().role == LlmRole::User &&
                        llm.requests[1].messages.back().content.find("station_search") !=
                            std::string::npos,
                    "the second call must feed the tool observations back to the model");
        tests.check(llm.requests.size() == 2 && llm.requests[1].messages.back().content.find(
                                                    "不要编造") != std::string::npos,
                    "the summary prompt must forbid fabrication");
    }

    if (tests.result() == 0)
        std::cout << "agent service tests passed\n";
    return tests.result();
}
