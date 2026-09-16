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
  API 启动会自动执行迁移；独立部署时应先确认 `schema_migrations` 已包含当前二进制嵌入的全部迁移（当前最高版本 12）。
- 开发种子数据：`seeds/dev_seed.sql`，仅用于开发库，禁止用于预发/生产：

  ```bash
  psql "<开发库 DSN>" -f backend/seeds/dev_seed.sql
  ```
- L4 高容量平台级档案（仅开发/压测库，禁止用于预发/生产）：
  `seeds/dev_l4_seed.sql`。默认规模对齐平台级指标——**注册用户 1,000 万、
  充电站 1 万、充电设备 10 万**（每站 4 直流 + 6 交流，约 1% 故障），并为
  合成账号生成钱包（每 20 个账户余额为 0，覆盖欠费路径）。脚本幂等可重跑，
  全量执行约 2~6 分钟：

  ```bash
  psql "<开发库 DSN>" -f backend/seeds/dev_l4_seed.sql
  # 小规模冒烟档：
  psql "<开发库 DSN>" -v l4_users=10000 -v l4_stations=20 -f backend/seeds/dev_l4_seed.sql
  ```

  合成数据口径：用户手机号固定使用 `138` + 8 位数字（`13800000000` 起），
  站点编码 `L4S` + 5 位数字，全部口令使用与 `dev_seed.sql` 相同的开发专用
  固定哈希；该数据不得离开开发环境。注意 L4 规模下管理端关键字检索
  （`ILIKE '%kw%'`）为顺序扫描，首次验证性能时预期偏慢。

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
| `NCS_BILLING_TZ` | Asia/Shanghai | 分时电价（谷时）窗口按哪个时区的墙上时间判定；计费时刻本身仍是 UTC。**不要设成 UTC**：运营配置的 23:00-07:00 指本地夜间，按 UTC 判定会把折扣挪到本地白天 |
| `NCS_CHARGER_EVENT_MAX_FUTURE_SKEW` | 5m | 设备事实时间可超前服务器时钟的上限 |
| `NCS_STOP_RECOVERY_MAX_ATTEMPTS` | 3 | STOP 命令总尝试次数上限 |
| `NCS_STOP_RECOVERY_BACKOFF` | 5m | 故障 STOP 重发间隔下限 |
| `NCS_ENV_FILE` | `backend/.env.local` | `local-stack.sh` 启动前加载的本地配置文件；用于 Agent 地图和模型凭据，不提交到仓库 |
| `TENCENT_MAP_SERVER_KEY` | 空 | Agent 服务端地图 Key；为空时地图工具降级 |
| `AI_PROVIDER` / `AI_MODEL` / `AI_BASE_URL` / `AI_API_KEY` | 空 | Agent 模型配置；为空时仍使用实时站点数据确定性作答，并返回 `degraded=true` |

本地栈会自动加载 `backend/.env.local`，可从 `.env.example` 开始配置。这样从不同终端
重启服务时不会再丢失 Agent 的地图和模型环境变量；真实凭据不得提交到仓库。

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
| `NCS_MOCK_GATEWAY_METER_INTERVAL` | 3s | 开启自动回执后，充电中周期上报运行计量的间隔（`CHARGE_PROGRESS`）；设为 `0` 可单独关闭实时计量 |
| `NCS_MOCK_GATEWAY_CHARGE_SECONDS` | 60 | 模拟一次充电达到 `ENERGY_WH` 所需的秒数；运行计量在这段时间内线性爬升，之后停止上报 |

### 运行计量（充电中的实时电量与金额）

平台只在**停止回执**里拿到计量时，App 在充电中只能显示占位符或估算。真实充电桩会在充电过程中
持续上报运行计量（现场即 OCPP 的 MeterValues），因此契约里多了一种回执：

```text
CHARGE_PROGRESS  { orderNo, chargerId, energyWh(已充电量,绝对值), occurredAt, eventId }
```

- 只对 `CHARGING` 的订单生效；读数**单调不回退**，比已存读数更小的投递按"迟到"忽略并返回当前状态，
  因此它不需要幂等键（绝对值的重复写入天然幂等，幂等表也不会每条读数长一行）。
- 平台把读数存进 `metered_energy_wh/metered_at`，并在读取订单详情时用**与最终账单同一个分时引擎**
  算出 `meteredAmountCent`：客户盯着的数字会收敛到发票金额，而不是另一套算法。
- 实时值（`meteredEnergyWh/meteredAmountCent/meteredAt`）与结算值（`energyWh/amountCent`）**分开**：
  实时读数绝不写进结算金额，结算仍由停止回执决定，停止后实时字段消失。
- 开启 `NCS_MOCK_GATEWAY_METER_INTERVAL` 后，模拟网关按该间隔上报，且**停止时按已计量的电量结算**
  （否则会出现"充电中 0.25 kWh、结算 1.00 kWh"这种设备不可能给出的数字）。

`mock-gateway` 单独启动时回执仍默认关闭：`backend/scripts/verify-closed-loop.sh`
断言“设备接受命令本身不改变订单状态”，并自行上报回执以便把设备事实时间放进指定费率时段。
用于实际页面联调的 `backend/scripts/local-stack.sh` 与 Compose `mock` profile 默认开启自动回执和 3 秒计量，
使 START/STOP 与实时金额形成完整闭环；需要验证半闭环语义时可显式设置
`NCS_MOCK_GATEWAY_RECEIPTS=false`。

设备回执以命令号为幂等键：同一命令重复投递只上报一次，平台拒绝的上报会在该命令下一次
投递时以**完全相同的报文**重试（回执 id 与事实时间首次决定后不再变化，否则重试会变成冲突）。

## 运营统计

旧 Qt 管理端的驾驶舱数字来自服务端预计算的快照（`DashboardService` + `station_hourly_metric`）。
Go 侧改为**按需聚合**，不再维护第二份事实来源：PostgreSQL 对订单表的聚合比一次快照重建更快，
而且数字永不陈旧。

三个端点（均在 `internal/admin`，管理员会话可读，审计员亦可）：

| 端点 | 用途 | 口径 |
|---|---|---|
| `GET /admin/stats/revenue` | 营收趋势、每日/每小时明细 | 已完成订单的冻结金额，归属 `stopped_at`；`fromAt`/`toAt` 必填、UTC 秒、区间 ≤90 天；`bucket=hour\|day` |
| `GET /admin/stats/chargers` | 电桩状态分布与健康度 | 全量设备（含停用）；`operationalCount`=空闲+在用，`healthPercent`=可运营/总数（两位小数） |
| `GET /admin/stats/overview` | 运营总览标量块 | 终身营收、完成充电数与其桩型构成、注册用户、站点数、在途订单、设备普查 |

两条口径是旧的业务规则，原样保留：**营收只算已完成订单的冻结金额**，取消与失败订单无论报价多少都不计入；
**分桶对齐请求区间起点**而不是 epoch——客户端给自己的本地日边界，只有对齐区间才能让一个桶正好等于客户端的一天。

空桶会补零：跳过安静的日子会让趋势图把两天连成不存在的走势。

## 站点编辑与全局费率

管理端的两个运营写入，都与既有端点同一套策略：读开放给全部管理员角色（审计员可读），
写只开放给可写角色。

**`PUT /api/v1/admin/stations/{stationId}`** —— 只改名称、地址与经纬度。编码是身份
（审计轨迹与订单历史都引用它），状态有自己的生命周期端点，两者都不在此接受；请求体里带上
也不会被采纳。坐标是 E6 整数，列里是度，转换在这层完成。变更与审计在同一事务里写入，
所以记录的 before/after 就是本次实际替换掉的那份。

**`GET /api/v1/admin/tariffs`** —— 车队当前持有的费率。价格存在电桩上而不是独立费率表，
因此返回的是真实状态：有几种配置、各多少桩，加上价格区间与分时/单一费率的分布。这个区间
就是下发前值得先看一眼的东西——全车队已经一致时下发是空操作，跨度过大时值得停一下。

**`PUT /api/v1/admin/tariffs`** —— 一次把**完整**费率写入所有电桩（含停用设备：今天停用的
设备明天会启用，留着去年的价格会由客户先发现）。请求体是完整费率而不是补丁，不带分时三项
即表示改为单一费率，与设备级端点同一规则。原因必填并写审计——这是本边界上影响面最大的写入，
"谁把平台价格改成这个、为什么"必须事后可回答。运行中的订单不受影响：订单域在开始充电时
快照费率。

> 旧的行政区价格版本（adcode + 生效时间窗）没有迁移。那是一个定价版本子系统：需要版本表、
> 生效时间解析与定时切换。这里做的是今天就能用的小东西，复用已有的列，不引入关于"一次充电
> 值多少钱"的第二份事实来源。

## AI 助手（A-07）

`POST /api/v1/agent/chat` 是浏览器访问助手的唯一入口，需要用户会话。

助手是**只读**的：它读取站点、周边地点与路线，不写订单、充电桩或余额。请求体只有问题、
自己的位置与可选桩型偏好；地图 Server Key、模型凭据、工具编排与降级策略全部留在服务端。

服务端**不因模型或地图不可用而失败**。未配置时就按确定性意图判断选择工具、用规则文案作答，
并在响应里以 `degraded=true` 说明；`route.fallback=true` 表示路线是直线估算。

Agent 路由从进入认证起共享 **14 秒总预算**（包括请求体读取、模型和工具），
规划模型最多使用其中 **4 秒**，为真实站点查询留出时间。组织回答共享剩余预算；
超时保留已获取的结构化数据，返回 `degraded=true` 和明确文字提示，取消后不再启动后续调用。
路由预留 1 秒写响应，前端等待上限仍为 20 秒。`AI_TIMEOUT_MS` 只是单次调用上限，
不能延长总预算；数据库、地图和模型适配器必须遵守传入的 context。

| 变量 | 默认 | 说明 |
|---|---|---|
| `TENCENT_MAP_SERVER_KEY` | 空 | 腾讯地图 WebService 密钥。为空时 POI/路线不可用，助手降级但仍能用站点数据作答 |
| `TENCENT_MAP_BASE_URL` | 空（官方主机） | 覆盖地图服务主机；用于沙箱、出口代理与联调 |
| `AI_PROVIDER` | openai | `openai` / `deepseek` / `qwen` / `claude` / `custom` |
| `AI_MODEL` | 空 | 模型名。与 `AI_API_KEY` 任一为空即视为未配置模型 |
| `AI_API_KEY` | 空 | 模型凭据；只出现在请求头，绝不进日志、响应或前端产物 |
| `AI_BASE_URL` | 空（按 provider 预设） | 自定义端点必须显式设置；非 loopback 只接受 HTTPS |
| `AI_TIMEOUT_MS` | 15000 | 单次模型调用上限，客户端会再夹到 1000~60000 毫秒 |

四个工具（`station_search` / `station_detail` / `poi_search` / `route`）的参数一律按不可信输入
校验：越界、类型不符或缺失都不进入业务层。模型编造的工具名会被丢弃；模型只给结论不选工具时
由确定性判断补上计划；计划里缺少站点 ID 或目的地坐标时，服务端会先补一次站点检索建立锚点。

启动时会打印一行能力状态，便于确认当前部署处于哪一档：

```
msg="assistant capability" map_provider=false model=false tools="[station_search station_detail poi_search route]"
```

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
