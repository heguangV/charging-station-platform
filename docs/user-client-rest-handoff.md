# 用户端 REST 对接交接

## 范围

`apps/user/net/` 是 Qt 用户端唯一的 HTTP 边界。页面与 UI 组件不得手拼 URL、解析响应信封或直连 SQLite。

- `ApiClient`：异步 `QNetworkAccessManager`、10 秒超时、`Bearer` Token、`X-Request-ID`、统一 `success/code/userMessage/data` 解析。HTTP 4xx/5xx 仍会优先解析响应 JSON 中的 `code` 和 `userMessage`。
- `UserApi`：按 [database-api.md](database-api.md) 封装 `/api/v1/user` V1 路径、字段和分页参数。
- 充值和充电流程写操作生成 `Idempotency-Key`。对一次 JSON 请求，超时或可恢复断线会在 250ms 后自动重试一次，保留原始 Idempotency-Key 与 X-Request-ID；不自动重试 multipart 头像上传，因为上传流已被消费。

## 快速联调

使用后端地址配置后，可在不启动业务窗口的情况下验证验证码路由：

```bash
NCS_ENVIRONMENT=development NCS_LISTEN_ADDRESS=127.0.0.1 \
NCS_PORT=8443 NCS_ALLOW_INSECURE_HTTP=true ./build/dev/ncs_server

NCS_SERVER_HOST=127.0.0.1 NCS_SERVER_PORT=8443 NCS_ALLOW_INSECURE_HTTP=true \
  ./build/dev/apps/user/ncs_user --api-request-code 13800138000
```

第一条命令只会在 `development` + 数字回环地址下启用服务端 HTTP；其他环境或地址会拒绝启动并保持失败关闭。第二条命令调用 `POST /api/v1/user/auth/sms/code`，请求体包含 `phone` 与 `purpose=LOGIN`。成功退出码为 0；失败输出服务端 `userMessage` 并以 4 退出。该模式仅用于同机开发联调，不得用于测试、验收、生产、局域网或公网。

## 当前状态

2026-09-06 回归：登录成功后才请求站点；返回首页立即刷新，停留首页每 5 秒异步更新空闲数量，忽略过期响应。隔离后端真实 HTTP 验证了零余额拒绝预约（错误码 7）、充值、预约、充电、结算及小票，并断言充电时空闲数减少 1、结算后恢复。

本轮 `ncs_user_validate`、正式 `ncs_user` 构建及离屏烟雾检查通过，网络与 Mock 单元测试通过。仓库行数、空白检查通过；本机没有 clang-format，格式检查未执行。地图找站页为后端坐标示意图，不是道路底图。最新 GUI 全页面截图验收仍待补充。

正常启动会创建 `ApiClient` 和 `UserApi`，页面以异步 REST 调用真实服务端：验证码登录、站点和电桩查询、充电流程、订单、资料、头像、充值、注销及路线查询均已接入。`MockUserClientService` 仅在显式传入 `--mock-scenario` 时用于离线演示和回归测试，不是服务端不可用时的自动降级，避免把演示数据误认为业务事实。

取得会话后，导航页调用 `/stations/{stationId}/route`，并使用所选站点的实际经纬度；腾讯地图路线或内嵌地图不可用时才退回浏览器路线。本机隔离服务端已完成真实 HTTP API 闭环验收：验证码登录、充值、站点/电桩查询、预约、开始充电、进度、结算、订单列表和小票均已通过。Qt 图形界面逐页点击录屏、运营电站数据导入及腾讯地图外部路线验收仍需单独执行。自签名 TLS 不调用 `ignoreSslErrors()` 绕过校验；开发环境应使用受信任的开发 CA，或显式使用仅限回环地址的本地 HTTP 开关。
