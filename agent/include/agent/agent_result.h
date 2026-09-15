// Agent 对外结果值类型：自然语言回复 + 结构化数据 + 可执行动作 + 降级标志。
// 结构化部分直接复用 core 应用服务的站点模型，保证与既有 REST 契约同源，前端可用同一套
// 卡片组件渲染；空集合表示“确实没有结果”，调用方不得用具空数据的对象冒充成功。
// 约束：本结构不含任何密钥、SQL、内部路径或完整手机号；route 为 nullopt 表示本次未规划路线。

#pragma once

#include "core/application/navigation_service.h"
#include "core/application/poi_service.h"
#include "core/application/station_service.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ncs::agent
{

// POI 结果直接复用 core 端口的 POI 数据模型，避免同一结构在 core 与 agent 两处漂移。
using AgentPoi = core::application::PoiItem;

struct AgentRouteStep
{
    std::string instruction;
    std::int64_t distanceMeter = 0;
    std::int64_t durationSecond = 0;
};

struct AgentRoute
{
    std::string destinationName;
    std::int64_t originLatitudeE6 = 0;
    std::int64_t originLongitudeE6 = 0;
    std::int64_t destinationLatitudeE6 = 0;
    std::int64_t destinationLongitudeE6 = 0;
    std::int64_t distanceMeter = 0;
    std::int64_t durationSecond = 0;
    std::string provider;
    bool fallback = false;
    std::vector<AgentRouteStep> steps;
    std::vector<core::application::RoutePoint> polyline;
    std::string browserUrl;
};

struct AgentAction
{
    // open_station：前端跳转站点详情；navigate：前端打开地图导航链接。
    std::string type;
    std::string label;
    std::string targetId;
    std::string url;
};

struct AgentResult
{
    std::string reply;
    std::vector<core::application::StationSummary> stations;
    std::vector<AgentPoi> pois;
    std::optional<AgentRoute> route;
    std::vector<AgentAction> actions;
    // 本次实际执行的工具名，按执行顺序，便于前端提示与端到端断言。
    std::vector<std::string> tools;
    // true 表示自然语言回复由 LLM 生成；false 表示走确定性兜底文案。
    bool llmUsed = false;
    // true 表示 LLM 或外部地图服务不可用、已按降级路径返回结果。
    bool degraded = false;
};

} // namespace ncs::agent
