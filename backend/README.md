# Go 后端迁移区

该目录承载新 Go API 和 Redis Streams Worker。

在 P0 闭环完成前，现有 C++/Crow 服务继续保留，不在本目录中复制或修改旧实现。

## 当前交付范围

本次 Go 后端 PR 包含 API、PostgreSQL 迁移与 Outbox、Redis/Streams Worker、
充电设备命令与内部回执入口。BE-I-02 的审批结论见
`docs/migration/be-i-02-charger-control-receipts-approval.md`；闭环验证使用模拟网关，
不代表真实设备协议已联调。

## PostgreSQL 迁移

- `migrations/`：递增 SQL 迁移，通过 `embed` 内嵌，API 启动时由
  `internal/repository/postgres.Run` 执行。整个迁移过程在**单个保留连接**上
  完成（会话级 advisory lock + 每文件独立事务），`NCS_POSTGRES_MAX_CONNS=1`
  也能安全启动。重复执行幂等，篡改已应用迁移会被拒绝。
- 启动新 Worker 前必须先应用顺序迁移 `0007_charger_command_outcomes.sql`。
  API 启动会自动执行迁移；独立部署时应先确认 `schema_migrations` 已包含版本 7。
- 开发种子数据：`seeds/dev_seed.sql`，仅用于开发库，禁止用于预发/生产：

  ```bash
  psql "<开发库 DSN>" -f backend/seeds/dev_seed.sql
  ```

## 运行 API

```bash
export NCS_POSTGRES_DSN="postgres://ncs:ncs@127.0.0.1:5432/ncs_dev"
export NCS_REDIS_ADDR="127.0.0.1:6379"
export NCS_CHARGER_GATEWAY_TOKEN="<从安全配置注入的网关服务令牌>"
go run ./cmd/api
```

关键环境变量：

| 变量 | 默认 | 说明 |
|---|---|---|
| `NCS_POSTGRES_DSN` | 必填 | PostgreSQL 连接串，缺失时进程拒绝启动 |
| `NCS_POSTGRES_MAX_CONNS` | 10 | 连接池上限（1 也安全） |
| `NCS_REDIS_ADDR` 等 | 127.0.0.1:6379 | Redis 连接，见 `repository/redis` 的 `NCS_REDIS_*`；必填，会话/限流/短信码依赖 |
| `NCS_SESSION_IDLE_TTL` | 30m | 会话空闲有效期 |
| `NCS_SESSION_ABSOLUTE_TTL` | 168h | 会话绝对有效期（需求下限 7 天，不可低于） |
| `NCS_LOGIN_RATE_LIMIT` | 10 | 登录限流：窗口内每账号尝试次数（Redis 共享） |
| `NCS_LOGIN_RATE_WINDOW` | 1m | 登录限流窗口 |
| `NCS_SMS_MOCK` | development 为 true | 模拟短信：验证码直接返回给客户端；正式环境必须关闭 |
| `NCS_CHARGER_GATEWAY_TOKEN` | 必填、无默认值 | 内部设备回执端点的 Bearer 服务令牌，缺失时 API 拒绝启动 |
| `NCS_CHARGER_EVENT_MAX_FUTURE_SKEW` | 5m | 设备事实时间可超前服务器时钟的上限 |
| `NCS_STOP_RECOVERY_MAX_ATTEMPTS` | 3 | STOP 命令总尝试次数上限 |
| `NCS_STOP_RECOVERY_BACKOFF` | 5m | 故障 STOP 重发间隔下限 |

正式部署还须通过 HTTPS 提供回执入口，并由 Nginx 限制
`/api/v1/internal/charger-events` 仅网关来源网络可达；令牌不能交给 H5 客户端。

## 登录与账号

- 用户默认入口为**短信免密登录**（UC-U-01）：
  `POST /api/v1/auth/user/sms/code` 获取验证码（开发环境在响应中返回模拟码），
  `POST /api/v1/auth/user/login/sms` 登录；首次登录自动注册用户并创建零余额钱包。
- 密码登录（`/auth/user/login`、`/auth/admin/login`）为次要入口，不能替代短信登录。
- 会话保存在 Redis（`ncs:session:{id}`），多实例部署共享；任一实例签发的
  token 在全部实例有效，进程重启不清空会话。

## 模拟设备网关

`cmd/mock-gateway` 是开发用的设备替身：Worker 把充电命令发给它，它按冻结契约应答。
应答命令只覆盖闭环的一半——平台只在收到设备回执后才把订单从 `STARTING` 推进到
`CHARGING`、从 `STOPPING` 推进到 `COMPLETED`，所以只应答命令的网关会让订单永远停在
`STARTING`。

打开 `-receipts` 后，网关在充电命令完成后补上设备的另一半：向
`POST /api/v1/internal/charger-events` 上报 `CHARGE_STARTED` 或 `CHARGE_STOPPED`。

```bash
NCS_CHARGER_GATEWAY_TOKEN="dev-gateway-token" \
NCS_MOCK_GATEWAY_RECEIPTS=true \
go run ./cmd/mock-gateway
```

| 变量 | 默认 | 说明 |
|---|---|---|
| `NCS_MOCK_GATEWAY_RECEIPTS` | false | 是否上报设备回执；默认关闭，见下方说明 |
| `NCS_MOCK_GATEWAY_API_URL` | http://127.0.0.1:8080 | 回执上报目标 API 基址 |
| `NCS_CHARGER_GATEWAY_TOKEN` | 必填（开启回执时） | 回执端点的服务令牌，必须与 API 侧一致；缺失时进程拒绝启动 |
| `NCS_MOCK_GATEWAY_ENERGY_WH` | 1000 | 模拟停止时上报的计费电量（瓦时）；同时作为始末表底，保证三者自洽 |

回执默认关闭：`backend/scripts/verify-closed-loop.sh` 断言“设备接受命令本身不改变订单状态”，
并自行上报回执以便把设备事实时间放进指定费率时段。网关自动上报会悄悄改变该门禁证明的内容。

设备回执以命令号为幂等键：同一命令重复投递只上报一次，平台拒绝的上报会在该命令下一次
投递时以**完全相同的报文**重试（回执 id 与事实时间首次决定后不再变化，否则重试会变成冲突）。

## 集成测试

需要真实 PostgreSQL 的测试通过 `NCS_TEST_PG_DSN` 守卫，未设置时自动跳过。
全量验证还需单独指定测试 Redis 地址与数据库编号：

```bash
NCS_TEST_PG_DSN="<一次性测试库 DSN>" \
NCS_REDIS_TEST_ADDR="127.0.0.1:6379" NCS_REDIS_TEST_DB=15 \
go test -count=1 -race ./...
```

## 接口对接

客户端和 Agent 的接口基线见仓库根目录的 docs/api-integration.md，字段、枚举和响应结构以 api/openapi.yaml 为准。当前 Go 实际实现的路由与 OpenAPI 已对齐；订单确认接口未实现，也未登记在 OpenAPI 中。
