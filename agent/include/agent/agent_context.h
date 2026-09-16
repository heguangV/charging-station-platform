// Agent 一次对话的上下文：调用者身份、请求标识、用户位置与偏好。
// 由 server 的 Agent Controller 从已鉴权会话和请求 DTO 组装，AgentService 与各工具只读使用。
// 约束：只携带从 Bearer 令牌解析出的用户 ID，绝不接受客户端传入的用户标识；位置坐标统一
// E6 格式整数，坐标系由 wgs84Location 显式声明，避免把 WGS84 与 GCJ-02 混用。

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace ncs::agent
{

struct AgentContext
{
    // 当前用户 ID，来自会话令牌而非请求体。
    std::int64_t userId = 0;
    // 请求 ID，用于串联日志；不进入 Agent 对外的自然语言回复。
    std::string requestId;
    // 用户位置（E6 经纬度整数）。缺失表示“无定位”，工具必须走关键词地理编码或默认位置降级。
    std::optional<std::int64_t> latitudeE6;
    std::optional<std::int64_t> longitudeE6;
    // true 表示上面的坐标是浏览器 Geolocation 返回的 WGS84，参与腾讯地图服务前必须归一化为 GCJ-02。
    bool wgs84Location = false;
    // 用户显式指定的充电类型偏好：0 慢充 / 1 快充；缺失表示不限。
    std::optional<int> chargerType;
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();

    bool hasLocation() const
    {
        return latitudeE6.has_value() && longitudeE6.has_value();
    }
};

} // namespace ncs::agent
