# 用户端 REST 对接交接

## 范围

`apps/user/net/` 是 Qt 用户端唯一的 HTTP 边界。页面与 UI 组件不得手拼 URL、解析响应信封或直连 SQLite。

- `ApiClient`：异步 `QNetworkAccessManager`、10 秒超时、`Bearer` Token、`X-Request-ID`、统一 `success/code/userMessage/data` 解析。HTTP 4xx/5xx 仍会优先解析响应 JSON 中的 `code` 和 `userMessage`。
- `UserApi`：按 [database-api.md](database-api.md) 封装 `/api/v1/user` V1 路径、字段和分页参数。
- 充值和充电流程写操作生成 `Idempotency-Key`。对一次 JSON 请求，超时或可恢复断线会在 250ms 后自动重试一次，保留原始 Idempotency-Key 与 X-Request-ID；不自动重试 multipart 头像上传，因为上传流已被消费。

## 快速联调

使用后端地址配置后，可在不启动业务窗口的情况下验证验证码路由：

```bash
NCS_SERVER_HOST=127.0.0.1 NCS_SERVER_PORT=8443 NCS_ALLOW_INSECURE_HTTP=true \
  ./build/dev/apps/user/ncs_user --api-request-code 13800138000
```

该命令调用 `POST /api/v1/user/auth/sms/code`，请求体包含 `phone` 与 `purpose=LOGIN`。成功退出码为 0；失败输出服务端 `userMessage` 并以 4 退出。

## 当前状态

用户端窗口默认使用 `MockUserClientService`，保证服务端不可用时仍可演示完整流程。`UserApi` 已与团队当前契约对齐，但异步 REST Service/ViewModel 尚未注入现有同步 Mock 页面；该适配应作为下一独立任务完成，不能通过阻塞等待网络回复来替换 Mock。自签名 TLS 不调用 `ignoreSslErrors()` 绕过校验；开发环境应使用受信任的开发 CA，或显式使用本地 HTTP 开关，最终 HTTPS 方案由服务端证书链统一决定。
