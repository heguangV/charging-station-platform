# server：HTTP 服务、实时通知与运行时装配

`server` 构建平台后端程序 `ncs_server`，为桌面用户端、管理端、移动端和数据大屏提供 REST/WebSocket 接口。它接收请求、鉴权、转换协议、调用核心服务，并组织实时通知、周期维护及 ML 子进程。

业务规则位于 [`core/application`](../core/application/)，SQLite、地图和文件实现位于 [`infrastructure`](../infrastructure/)。服务端是这些模块的装配和运行入口。

## 目录和职责

| 位置 | 职责 |
| --- | --- |
| [`main.cpp`](main.cpp) | 解析参数，创建日志/会话/仓储/服务，注册路由，恢复流程，启动定时任务和监听，退出时释放资源 |
| [`server_app.h`](server_app.h) | 带请求策略中间件的 Crow 应用类型 |
| [`controller/`](controller/) | 路由、JSON DTO、响应信封、参数校验、异步派发和幂等响应包装 |
| [`middleware/`](middleware/) | Bearer 鉴权、角色和再认证检查、请求策略、限流、日志和全局异常处理 |
| [`runtime/`](runtime/) | 服务端配置、启动检查、TLS 上下文、周期调度和 ML 进程管理 |
| [`websocket/`](websocket/) | WebSocket 路由、连接适配、Outbox 投递和充电进度推送 |
| [`http/CMakeLists.txt`](http/CMakeLists.txt) | 定义公共响应库 `ncs_http_common`；实际源码位于 `controller/api_response.*` |

部分子目录旧 README 仍写有“预留目录”，当前已有实际实现；阅读时以本文件导航、源码和 CMake 编译清单为准。

## 请求执行链路

```text
客户端 HTTPS 请求
  → Crow / 请求策略中间件
  → Controller：参数、身份、角色和 DTO
  → 必要时交给 BoundedExecutor 执行阻塞工作
  → core 应用服务：业务状态检查和事务编排
  → infrastructure：SQLite / 地图 / 文件
  → 统一 JSON 响应

事务中的 Outbox → OutboxDispatcher → EventHub → WebSocket 客户端
```

`ApiRoutes` 管理 `/api/v1` 前缀，`api_response.*` 统一成功与失败响应。`async_response.*` 将后台工作派发到有界线程池，`idempotent_response.*` 连接幂等键、结果重放和业务调用。路由负责协议，核心服务负责业务，仓储负责持久化约束。

## 路由分组

| 文件 | 主要接口领域 |
| --- | --- |
| `health_routes.*` | 存活、就绪和系统健康信息 |
| `user_identity_routes.*` | 用户登录、凭据、资料、头像和注销 |
| `station_routes.*`、`navigation_routes.*` | 电站、设备和导航 |
| `flow_routes.*` | 排队、报价、预约、充电控制、订单和小票 |
| `order_payment_routes.*` | 用户确认扣款、申诉与管理端审核，由 `FlowRoutes` 调用注册 |
| `wallet_routes.*`、`order_review_routes.*` | 钱包、充值、流水、订单评价和评论墙 |
| `admin_routes.*` | 管理员认证/账号、用户、站点设备、价格和运维等 |
| `dashboard_routes.*`、`ml_routes.*` | 大屏数据、模型任务和内部数据交换 |

完整路径、字段、错误码、权限和幂等规则见[接口文档](../docs/database-api.md)，不在 README 中维护另一份完整 API 定义。

## 生命周期和后台任务

启动时检查配置与运行条件，创建 SQLite 仓储并执行迁移，装配服务，恢复活动流程和处理过期预约，随后注册接口并监听。

`PeriodicScheduler` 从 Crow 的统一 tick 驱动进度推送、Outbox、心跳、预约到期、设备命令完成、ML 超时、大屏快照及过期数据清理。耗时工作交给后台执行器；任务完成回调在异常路径也必须执行，避免某项周期任务永久停住。

`MlProcessManager` 管理 Python worker、工作令牌和进程生命周期。训练脚本在 [`ml/`](../ml/)，不在路由中执行训练。实时事件用于及时提醒，首次加载和断线恢复仍读取 REST 状态。

## 构建与本机启动

以下命令在**仓库根目录**执行。工程使用 C++17；开发预设要求 CMake 3.24+、Ninja，以及 Qt、OpenSSL、SQLite3。Crow 和 Asio 由根 CMake 的 FetchContent 配置获取。

```bash
cmake --preset dev
cmake --build --preset dev --target ncs_server
./build/dev/ncs_server --help
```

可执行文件位于构建目录根部 `build/dev/ncs_server`，不是 `build/dev/server/ncs_server`。使用其他构建目录时替换对应路径。

仅用于本机开发的回环 HTTP 示例：

```bash
./build/dev/ncs_server \
  --environment development \
  --listen-address 127.0.0.1 \
  --port 18443 \
  --allow-insecure-http true \
  --database-path /tmp/ncs-local-dev.db \
  --log-directory /tmp/ncs-local-dev-logs
```

```bash
curl --fail http://127.0.0.1:18443/api/v1/system/health/ready
```

该命令创建或使用指定数据库，开发验证应选择独立路径。正式环境使用受信任 TLS 配置；参数、管理员初始化、部署及恢复步骤以[运维手册](../docs/operations-guide.md)和 `--help` 为准。真实密钥、令牌和私钥不写入文档。

## 验证与扩展

从根目录构建测试后执行：

```bash
cmake --build --preset dev --target ncs_user_business_routes_tests ncs_order_payment_tests ncs_admin_routes_tests ncs_websocket_dispatcher_tests
ctest --test-dir build/dev --output-on-failure -R '^(ncs_user_business_routes|ncs_order_payment|ncs_admin_routes|ncs_websocket_dispatcher)$'
```

完整验证还包括 `ncs_server_smoke`、配置、鉴权、幂等、WebSocket 和运行时测试，入口在 [`tests/`](../tests/)。新增接口时同步服务、路由注册、DTO、文档和测试；涉及账户或订单时检查身份、所有权和业务状态。数据库和外部网络工作不能阻塞 Crow 事件循环，也不能绕过核心服务在路由中直接实现扣款。
