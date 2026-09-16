// 用户端 Agent 路由（POST /user/agent/chat）：把已鉴权请求转换为 Agent 上下文，调用
// AgentService，并把结构化结果序列化为 DTO。
// 职责边界：本控制器只做 HTTP、鉴权、DTO 转换和参数校验；意图识别、工具编排与大模型调用
// 全部位于 agent 模块，控制器不得内联任何推理或工具逻辑。
// 约束：请求体中的 message 与坐标是不可信输入，必须逐项校验；用户 ID 只取自 Bearer 会话；
// 响应不含密钥、SQL、内部路径或完整手机号；本接口只读，不产生业务写入，因此不要求幂等键。

// 注意包含顺序：Crow 必须先于任何 Qt 头文件被解析。Qt 在未定义 QT_NO_KEYWORDS 时会把
// signals/slots/emit 定义为宏，若先包含 Qt 再包含 Crow，Crow 自身的 signals() 成员会被
// 宏改写而编译失败。api_routes.h 间接引入 Crow，因此必须排在 agent 头文件之前。

#pragma once

#include "core/application/bounded_executor.h"
#include "core/application/session_manager.h"
#include "server/controller/api_routes.h"

#include "agent/agent_service.h"

namespace ncs::server::controller
{

class AgentController final
{
  public:
    AgentController(ApiRoutes& routes, agent::AgentService& agent,
                    core::application::SessionManager& sessions,
                    core::application::BoundedExecutor& executor);
};

} // namespace ncs::server::controller
