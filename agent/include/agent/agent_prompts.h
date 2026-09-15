// Agent 提示词与固定文案：集中定义系统提示词和降级路径文案，使 AgentService 只负责编排，
// 不把大段自然语言散落在逻辑代码里。
// 约束：提示词必须显式要求模型不暴露内部标识、密钥、SQL、内部路径与完整手机号，不承诺
// 价格或空闲数量，并在没有工具数据时如实说明，不得编造站点、POI 或路线。

#pragma once

#include <string_view>

namespace ncs::agent
{

// 系统提示词：限定角色、可用数据来源、输出要求与安全边界。
std::string_view agentSystemPrompt();

// LLM 未配置或调用失败时的统一说明前缀（确定性兜底文案会在此基础上补充实际数据）。
std::string_view agentLlmUnavailableNotice();

// 工具确实返回空结果时的说明。
std::string_view agentNoStationNotice();
std::string_view agentNoPoiNotice();

// 缺少用户位置且关键词地理编码也失败时的说明。
std::string_view agentNoLocationNotice();

// 外部地图服务不可用时的说明。
std::string_view agentMapUnavailableNotice();

} // namespace ncs::agent
