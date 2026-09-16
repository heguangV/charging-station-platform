# Go 路由与 OpenAPI 对照矩阵（前后端联调 第 1 阶段 · B 线交付物）

- 分支：`codex/backend/frontend-integration`（基线 `origin/develop` @ `422ee83`）
- 依据：`docs/integration/frontend-go-integration-task-plan.md` 第 5 节（第 1 阶段：B 线输出 Go 路由和 OpenAPI 对照）、B-01、B-02
- 本文件回答的问题：**前端要调的每个接口，Go 是否真的存在、是否已登记 OpenAPI、鉴权与幂等要求是什么、两边是否一致。**
- 状态口径（第 4 节"第二阶段：契约冻结"）：`MATCH` / `FRONTEND_CHANGE` / `BACKEND_CHANGE` / `BLOCKED`。
  只有 `MATCH` 项才能进入页面联调。

## 0. 事实来源（可复现）

| 事实 | 取法 |
| ---- | ---- |
| Go 实际注册的路由与鉴权包装 | `grep -rhoE 'Register\("[^"]+", *[^)]*\)' cmd/api internal --include='*.go' --exclude='*_test.go'`（36 条产品路由 + `order.ChargerEventPath` 常量注册的 `/api/v1/internal/charger-events`） |
| 探测端点 | `internal/httpapi/server.go:84-85`：`/healthz`、`/readyz` 注册在**根路径** |
| OpenAPI 操作 | `python3 -c "import yaml; yaml.safe_load(open('api/openapi.yaml'))"` → 45 个操作 / 38 条路径，`servers: ['/api/v1']`，全局 `security: [bearerAuth]` |
| 实机验证 | `local-stack.sh --seed` 起全栈后 `login ok as the seeded user, 1 station(s) visible` |

## 1. 全局契约（前后端都要按这个来）

| 项 | 约定 | 出处 |
| -- | ---- | ---- |
| 响应信封 | `{success, code, message, data}` | `internal/httpapi` |
| 成功 | `success=true, code=0` | 同上 |
| 错误码 | 0 OK／1 INVALID_ARGUMENT(400)／3 DATABASE_ERROR(503)／4 NOT_FOUND(404)／6 USER_FROZEN(403)／14 IDEMPOTENCY_CONFLICT(409)／18 ORDER_NOT_REFUNDABLE(409)／19 RATE_LIMITED(429)／401 UNAUTHORIZED／403 FORBIDDEN／1001 NOT_READY／1002 METHOD_NOT_ALLOWED／1003 NOT_FOUND（路由级）／1500 INTERNAL | `internal/httpapi/server.go` 常量 |
| 鉴权 | `Authorization: Bearer <accessToken>`；OpenAPI 全局 `bearerAuth`，公开接口逐条 `security: []` | `api/openapi.yaml`、`internal/auth` |
| 管理端权限 | 读：`AUDITOR` 亦可；写：`OPERATOR`/`SUPER_ADMIN`（`RequireAdminWrite`）；普通 USER 不得进入 `/admin/*` | `internal/auth` 中间件 + 各 `Register` 包装 |
| 请求 ID | 客户端可传 `X-Request-ID`，服务端回显并有日志字段；已在 B-06 打通 | `internal/httpapi`、`cmd/api/observability.go` |
| 分页 | 请求 `page`/`pageSize`（≤100）；响应 `data.items[]` + `data.meta{page,pageSize,total}` | `components/parameters/{Page,PageSize}` |
| 金额 | 一律**整数分**（`amountCent`/`balanceCent`/`paidCents` 等），前端不做浮点运算 | `internal/wallet`、`internal/order` |
| 时间 | RFC3339、UTC（服务端 `time.Time` 序列化为 `2026-09-15T11:18:08Z`） | 各响应 DTO |
| 幂等 | 需要幂等的写操作必须带 `Idempotency-Key`（16..128 字符）；重复键**同请求**返回首次结果，**不同请求**返回 409/code 14 | `components/parameters/IdempotencyKey`、`internal/repository/postgres` |
| 设备网关令牌 | **只**出现在网关与 API 之间（`chargerGatewayBearer`）；前端不得持有、日志不得打印 | `api/openapi.yaml`、`deploy/README.md` |

## 2. 对照矩阵

状态含义：`MATCH` = 路径/方法/鉴权/信封一致，可进入联调；`BACKEND_CHANGE` = B 线需改；`FRONTEND_CHANGE` = 前端需改；`BLOCKED` = 依赖未实现能力（A-07/Agent）。

| 方法 | 路径（`servers=/api/v1`） | Go 注册与鉴权 | OpenAPI | 幂等 | 状态 |
| ---- | ------------------------- | ------------- | ------- | ---- | ---- |
| POST | `/auth/user/login` | `h.UserLogin`，公开 | ✅ | – | MATCH |
| POST | `/auth/user/sms/code` | `h.RequestSMSCode`，公开 | ✅ | – | MATCH |
| POST | `/auth/user/login/sms` | `h.SMSLogin`，公开 | ✅ | – | MATCH |
| POST | `/auth/admin/login` | `h.AdminLogin`，公开 | ✅ | – | MATCH |
| POST | `/auth/logout` | `h.Logout`，Bearer | ✅ | – | MATCH |
| GET | `/me` | `h.RequireIdentity(h.meRoutes)`，Bearer | ✅ | – | MATCH |
| DELETE | `/me` | 同上（同路由多方法） | ✅ | – | MATCH（注销语义见 A-01 遗留项：重试应返回 204） |
| GET | `/me/profile` | `h.RequireIdentity(h.Profile)` | ✅ | – | MATCH |
| PUT | `/me/profile` | 同上 | ✅ | – | MATCH |
| GET | `/stations` | `RequireIdentity(ListStations)` | ✅ | – | MATCH |
| GET | `/stations/{stationId}` | `RequireIdentity(GetStation)` | ✅ | – | MATCH |
| GET | `/stations/{stationId}/reviews` | `RequireIdentity(wall)` | ✅ | – | MATCH |
| GET | `/chargers` | `RequireIdentity(ListChargers)` | ✅ | – | MATCH |
| GET | `/wallet` | `RequireIdentity(view)` | ✅ | – | MATCH |
| POST | `/wallet/top-up` | `RequireIdentity(topUp)` | ✅ | ✅ | MATCH（金额 1..1000000 分） |
| GET | `/wallet/transactions` | `RequireIdentity(transactions)` | ✅ | – | MATCH（分页 + `type` 过滤） |
| POST | `/orders` | `RequireRole(RoleUser, orders)` | ✅ | ✅ | MATCH |
| GET | `/orders` | 同上 | ✅ | – | MATCH（分页 + 状态/站点过滤） |
| GET | `/orders/{orderNo}` | `RequireRole(RoleUser, getOrder)` | ✅ | – | MATCH |
| POST | `/orders/{orderNo}/start` | `RequireRole(RoleUser, startCharging)` | ✅ | ✅ | MATCH |
| POST | `/orders/{orderNo}/stop` | `RequireRole(RoleUser, stopCharging)` | ✅ | ✅ | MATCH |
| POST | `/orders/{orderNo}/cancel` | `RequireRole(RoleUser, cancelOrder)` | ✅ | ✅ | MATCH |
| GET | `/orders/{orderNo}/review` | `RequireIdentity(reviewRoutes)` | ✅ | – | MATCH |
| POST | `/orders/{orderNo}/review` | 同上 | ✅ | – | MATCH |
| POST | `/orders/{orderNo}/appeal` | `RequireIdentity(createAppeal)` | ✅ | – | MATCH |
| GET | `/admin/users` | `RequireRole(RoleAdmin, listUsers)` | ✅ | – | MATCH |
| GET | `/admin/users/{userId}` | `RequireRole(RoleAdmin, userDetail)` | ✅ | – | MATCH |
| POST | `/admin/users/{userId}/freeze` | `RequireAdminWrite(freezeUser)` | ✅ | – | MATCH |
| POST | `/admin/users/{userId}/unfreeze` | `RequireAdminWrite(unfreezeUser)` | ✅ | – | MATCH |
| GET | `/admin/users/{userId}/transactions` | `RequireRole(RoleAdmin, userLedger)` | ✅ | – | MATCH |
| GET | `/admin/orders` | `RequireRole(RoleAdmin, listOrders)` | ✅ | – | MATCH |
| POST | `/admin/orders/{orderNo}/refund` | `RequireAdminWrite(refundOrder)` | ✅ | ✅ | MATCH（订单不存在→404/code 4；已退款→409/code 18） |
| GET | `/admin/stations` | `RequireRole(RoleAdmin, stations)` | ✅ | – | MATCH |
| POST | `/admin/stations` | 同上（同路由多方法） | ✅ | ✅ | MATCH |
| GET | `/admin/chargers` | `RequireRole(RoleAdmin, listChargers)` | ✅ | – | MATCH |
| GET | `/admin/chargers/{chargerId}/tariff` | `h.tariffRoutes` | ✅ | – | MATCH |
| PUT | `/admin/chargers/{chargerId}/tariff` | 同上 | ✅ | – | MATCH |
| POST | `/admin/chargers/{chargerId}/release` | `RequireAdminWrite(forceRelease)` | ✅ | ✅ | MATCH |
| POST | `/admin/chargers/{chargerId}/restart` | `RequireAdminWrite(restartCharger)` | ✅ | ✅ | MATCH |
| GET | `/admin/appeals` | `RequireRole(RoleAdmin, adminListAppeals)` | ✅ | – | MATCH |
| POST | `/admin/appeals/{appealId}/approve` | `RequireAdminWrite(adminApprove)` | ✅ | – | MATCH |
| GET | `/admin/audit` | `RequireRole(RoleAdmin, listAudit)` | ✅ | – | MATCH |
| POST | `/internal/charger-events` | `order.ChargerEventPath` + 网关令牌校验 | ✅（`security: chargerGatewayBearer`） | – | MATCH（**仅网关调用，前端不得调用**） |
| GET | `/healthz` | **根路径** `/healthz`（`internal/httpapi/server.go:84`） | ✅ 但挂在 `servers=/api/v1` 下 | – | **BACKEND_CHANGE** |
| GET | `/readyz` | **根路径** `/readyz`（同文件 85 行） | ✅ 同上 | – | **BACKEND_CHANGE** |
| – | `POST /api/v1/user/agent/chat` | 未实现、未登记 | ❌ | – | **BLOCKED**（A-05/A-07，不得伪装接通） |

**结论**：产品路由共 37 条，OpenAPI 共 45 个操作，除下面两处外一一对应；**没有"Go 有而 OpenAPI 没登记"的接口**，因此"前端依据未登记接口开发"的风险在后端侧为零。

> 注意口径：本表的 `MATCH` 指 **Go 路由 ↔ OpenAPI** 一致（后端侧契约自洽）。**前端 ↔ Go** 的对接状态是另一件事，见第 7 节：按实际代码核对后，前端调用与 Go 契约完全一致的条目为 **0**。

## 3. 差异清单（需要处理的）

### D-1 `BACKEND_CHANGE`：`/healthz`、`/readyz` 的路径前缀不一致 —— **已修**

- 事实：OpenAPI 全局 `servers: [{url: /api/v1}]`，而这两个操作没有 `servers` 覆盖 → 按规格解析出的地址是 `/api/v1/healthz`、`/api/v1/readyz`；Go 实际只在**根路径**提供，且部署口径（nginx、systemd、探针）用的也是根路径。
- 影响：按规格生成的客户端会去调 `/api/v1/healthz` 并拿到 404。
- 处理（已实施并验证）：给这两个操作加 `servers: [{url: /}]`，解析结果回到 `/healthz`、`/readyz`，与 `internal/httpapi/server.go:84-85` 一致；`/metrics` 不登记 OpenAPI（B-06 口径，运维端点不暴露给前端）。

### D-2 `BLOCKED`：Agent 对话接口

- `agent/` 需要 `POST /api/v1/user/agent/chat`，Go 未实现也未登记。按任务文档 A-05：A 线只做请求层与降级 UI，**不得标记为已接通**；B 线在 A-07 或独立 Agent 模块审批后再登记 OpenAPI 并实现。

### D-3 前端清单已核对（原为"待 A 线清单到位后比对"）

fork `cjyhjy/charging-station-platform` 分支 `feat/postgres-agent-web-migration`（`4d1a265`）已按只读方式克隆并逐条盘点，结论见第 7 节；明细见 `frontend-api-inventory.md`。

核心发现：前端请求层（`apps/user/src/api/http.js`、`apps/admin/src/api/http.js`）的**传输约定与 Go 完全一致**（`API_BASE=/api/v1`、Bearer、`X-Request-ID`、`Idempotency-Key`、`{success,code,message,data}` 信封），但**路径与分页字段仍是旧契约**：前端用 `/user/...`，Go 用 `/me`、`/orders`……；前端读 `data.total`，Go 返回 `data.meta.total`。

关键旁证：同一 fork 的 `docs/api-integration.md` 开头即写明"本文档与当前 Go 后端实现保持一致……不要根据旧 C++ 接口自行推断路径或字段"，其列出的路径正是 Go 的路径——**代码尚未按该文档迁移**。

## 4. 本轮已完成的 B 线开工项

### 4.1 `local-stack.sh --seed` 迁移顺序（任务文档 §9 指定）

修复前：seed 在迁移门禁**之前**执行，对一次性空库直接失败——

```text
=== seed development data into ncs_fe_repro ===
psql:.../dev_seed.sql:14: ERROR:  relation "admin_accounts" does not exist
psql:.../dev_seed.sql:18: ERROR:  relation "user_accounts" does not exist
psql:.../dev_seed.sql:25: ERROR:  relation "wallet_accounts" does not exist
psql:.../dev_seed.sql:29: ERROR:  relation "stations" does not exist
```

修复后（顺序调整 + `psql -v ON_ERROR_STOP=1` 让半成功也暴露）：

```text
=== migration gate ===
level=INFO msg="database migrations applied" count=9
=== seed development data into ncs_fe_repro ===
=== start mock gateway, API, publisher and worker ===
=== smoke ===
login ok as the seeded user, 1 station(s) visible
=== local stack is up ===
```

### 4.2 迁移门禁缺少网关令牌（同一次修复中暴露）

`common_env` 没有 `NCS_CHARGER_GATEWAY_TOKEN`，而 `-migrate-only` 跑的就是 API 二进制、启动即校验配置 → 空库上迁移门禁直接失败（`NCS_CHARGER_GATEWAY_TOKEN is required`），脚本第 20 行文档承诺的默认值从未生效。已把令牌并入 `common_env`，由上面的实测结果确认。

## 5. 本轮实测（B-03 部署与 Nginx / B-04 WSL 本地栈）

一次性库 `ncs_fe_nginx` 从空库起栈，命令：

```bash
NCS_POSTGRES_DSN=postgres://.../ncs_fe_nginx bash backend/scripts/local-stack.sh --seed --with-nginx
```

原始输出（节选）：

```text
=== migration gate ===        level=INFO msg="database migrations applied" count=9
=== seed development data into ncs_fe_nginx ===
=== start mock gateway, API, publisher and worker ===
=== smoke ===                 login ok as the seeded user, 1 station(s) visible
=== render and start nginx ===  nginx: configuration file ... syntax is ok
                                configuration file ... test is successful
                                nginx: static=200 /healthz via proxy=200
=== local stack is up ===
```

随后逐条核对 B-03 的验收项（经 Nginx，自签证书）：

| 验收项 | 实测 |
| ------ | ---- |
| H5 静态文件目录 | `GET / -> 200` |
| HTTP → HTTPS 301 | `GET http://localhost:8124/ -> 301 Location=https://localhost/` |
| `/healthz`、`/readyz` 经反代 | 均 `200` |
| `/api/` 反向代理 | 经 Nginx 登录成功（token 43 字符），`/api/v1/stations`、`/api/v1/wallet`、`/api/v1/orders` 均 `200` |
| TLS 自签本地验证 | 全程 `curl -k` 通过（证书由 `local-stack.sh` 现场生成） |
| 请求 ID 透传 | 发 `X-Request-ID: fedcba9876543210`，响应回显 `X-Request-Id: fedcba9876543210` |
| 设备回执来源限制 | 渲染出的 `location = /api/v1/internal/charger-events` 为 `allow 127.0.0.1; deny all;` |
| metrics/readyz 内网限制 | `location = /metrics`、`location = /readyz` 同样 `allow 127.0.0.1; deny all;`（本机回环属允许段故为 200；拒绝路径由 `nginx-render.sh --drill` 证明） |
| 不向前端暴露设备网关令牌 | `dev-gateway-token` 未出现在渲染配置、nginx 访问/错误日志、API/Worker 日志中（`grep` 计数 0） |

清理（任务文档 B-04：测试库必须一次性并在结束后删除）：

```text
一次性库 ncs_fe_repro、ncs_fe_nginx → 已 DROP（pg_database 查询结果为 none）
8080/8123/8124 端口 → 已释放        栈进程 → 无残留
（/tmp/ncs-stack-*/ 保留日志作为证据）
```

## 6. 下一步（B 线）

1. ~~**D-1**：给 OpenAPI 的两个探针操作加 `servers` 覆盖~~ **已完成**（见 D-1），并已核对解析结果；后续把这条解析检查纳入规格门禁脚本。
2. **B-05 联调脚本**：把"创建/清理一次性库 + 迁移 + seed + 起栈 + 生成 token + 核心 smoke + 检查日志是否泄露密钥 + 停止清理"做成一条可重复执行的入口（现在 `local-stack.sh` 需要外部提供 DSN，且停留在前台）。
3. **B-02 核心接口确认**：为 18 个核心接口逐项补齐 handler/错误/权限/真实 PG·Redis 测试与 OpenAPI 对照记录（多数已有测试，本项工作是**逐项登记与补齐缺口**，不是重写）。
4. **B-03 部署与 Nginx**：H5 静态目录、`/api/` 反代、301、自签 TLS、`/healthz`、`/readyz`、`/metrics`、请求 ID 透传、回执来源限制、metrics/readyz 内网限制、令牌不外泄。
5. **B-06 质量门禁**：`go build/vet/test/race`、真实 PG/Redis 测试、OpenAPI YAML 校验、`nginx -t`、`git diff --check`、无密钥与构建产物入库。
6. **等 A 线完成前端改造后进入页面联调**：第 7 节的 38 条 `FRONTEND_CHANGE` 是进入页面联调的前置条件（任务文档第 4 节：只有 `MATCH` 项才能进入页面联调）。其中"路径前缀/段序"与"分页 `data.total` → `data.meta.total`"是全局改动，改一次覆盖大部分页面；19 条 `BACKEND_CHANGE` 需先登记 OpenAPI 再实现，7 条 `BLOCKED` 按 A-05/A-07 处理。

## 7. 前端调用对照与契约冻结结论（基于 PR #42 fork 分支实测）

来源：fork `cjyhjy/charging-station-platform` 分支 `feat/postgres-agent-web-migration`（`4d1a265`），盘点明细见 `frontend-api-inventory.md`。
去重后的「方法+路径」共 **64** 条，分类结果：**BACKEND_CHANGE 19**、**BLOCKED 7**、**FRONTEND_CHANGE 38**。

> 分类规则（可复核）：用户端路径去掉 `/user` 前缀后若命中 Go 路由即判 `FRONTEND_CHANGE`（仍需改分页字段）；其余按显式例外表判定。**当前没有任何一条前端调用与 Go 契约完全一致**——`MATCH = 0`，按任务文档"只有 MATCH 项才能进入页面联调"，因此页面联调的前置条件是先完成下表的前端改造（或对 BACKEND_CHANGE 项补齐后端）。

### 7.1 用户端

| 方法 | 前端路径 | Go 对应 | 分类 | 依据 | 调用处 |
| ---- | -------- | ------- | ---- | ---- | ------ |
| POST | `/user/agent/chat` | **无** | BLOCKED | Go 未实现未登记；不得伪装接通 | `user/src/api/agent.js:16` |
| POST | `/user/auth/login/password` | `POST /auth/user/login` | FRONTEND_CHANGE | 同上；密码登录在 Go 属次要入口 | `user/src/api/auth.js:17` |
| POST | `/user/auth/login/sms` | `POST /auth/user/login/sms` | FRONTEND_CHANGE | 同上 | `user/src/api/auth.js:12` |
| POST | `/user/auth/logout` | `POST /auth/logout` | FRONTEND_CHANGE | 同上 | `user/src/api/auth.js:27` |
| POST | `/user/auth/register` | **无** | BACKEND_CHANGE | Go 无注册接口（当前仅短信登录隐式建号）；需契约裁定：补接口或前端去掉注册步骤 | `user/src/api/auth.js:22` |
| POST | `/user/auth/sms/code` | `POST /auth/user/sms/code` | FRONTEND_CHANGE | 段序不同，后端能力已具备 | `user/src/api/auth.js:7` |
| POST | `/user/flows` | `POST /orders` | FRONTEND_CHANGE | 流程模型映射到订单状态机 | `user/src/api/charging.js:7` |
| GET | `/user/flows/{flowNo}` | `GET /orders/{orderNo}` | FRONTEND_CHANGE | flowNo ↔ orderNo | `user/src/api/charging.js:17` |
| POST | `/user/flows/{flowNo}/cancellations` | `POST /orders/{orderNo}/cancel` | FRONTEND_CHANGE | 同上 | `user/src/api/charging.js:27` |
| GET | `/user/flows/{flowNo}/progress` | **无** | BACKEND_CHANGE | 无实时进度端点（详情有 energyWh/amountCent） | `user/src/api/charging.js:41` |
| POST | `/user/flows/{flowNo}/quote-confirmations` | **无** | FRONTEND_CHANGE | Go 无报价确认步骤；若产品要保留两步确认则升级 BACKEND_CHANGE（需裁定） | `user/src/api/charging.js:22` |
| POST | `/user/flows/{flowNo}/settlements` | **无** | FRONTEND_CHANGE | Go 在 STOP 回执确认时自动结算 | `user/src/api/charging.js:46` |
| POST | `/user/flows/{flowNo}/start` | `POST /orders/{orderNo}/start` | FRONTEND_CHANGE | 同上 | `user/src/api/charging.js:32` |
| GET | `/user/flows/active` | `GET /orders?status=...` | FRONTEND_CHANGE | 用订单状态过滤替代 active 概念 | `user/src/api/charging.js:12` |
| GET | `/user/me` | `GET /me` | FRONTEND_CHANGE | 前缀/形状调整后即可对应 | `user/src/api/auth.js:32` |
| PUT | `/user/me` | `PUT /me/profile` | FRONTEND_CHANGE | 路径与字段按 profile 结构对齐 | `user/src/api/auth.js:37` |
| POST | `/user/me/avatar` | **无** | BACKEND_CHANGE | Go 只存 avatarUrl 字符串（≤512），无上传/存储接口 | `user/src/api/auth.js:44` |
| GET | `/user/orders` | `GET /orders` | FRONTEND_CHANGE | 前缀/形状调整后即可对应 | `user/src/api/order.js:7` |
| GET | `/user/orders/{orderNo}` | `GET /orders/{orderNo}` | FRONTEND_CHANGE | 前缀/形状调整后即可对应 | `user/src/api/order.js:12` |
| GET | `/user/orders/{orderNo}/review` | `GET /orders/{orderNo}/review` | FRONTEND_CHANGE | 前缀/形状调整后即可对应 | `user/src/api/order.js:17` |
| POST | `/user/orders/{orderNo}/review` | `POST /orders/{orderNo}/review` | FRONTEND_CHANGE | 前缀/形状调整后即可对应 | `user/src/api/order.js:22` |
| GET | `/user/stations` | `GET /stations` | FRONTEND_CHANGE | 前缀/形状调整后即可对应 | `user/src/api/station.js:11` |
| GET | `/user/stations/{stationId}` | `GET /stations/{stationId}` | FRONTEND_CHANGE | 前缀/形状调整后即可对应 | `user/src/api/station.js:23` |
| GET | `/user/stations/{stationId}/chargers` | `GET /chargers?stationId=` | FRONTEND_CHANGE | 嵌套资源改为查询参数 | `user/src/api/station.js:28` |
| GET | `/user/stations/{stationId}/quote` | **无** | BACKEND_CHANGE | 无报价接口；金额由计费快照在创建订单时确定 | `user/src/api/station.js:33` |
| GET | `/user/stations/{stationId}/reviews` | `GET /stations/{stationId}/reviews` | FRONTEND_CHANGE | 前缀/形状调整后即可对应 | `user/src/api/station.js:53` |
| GET | `/user/stations/{stationId}/route` | **无** | BLOCKED | 导航/路线属 A-07 | `user/src/api/station.js:42` |
| GET | `/user/wallet` | `GET /wallet` | FRONTEND_CHANGE | 前缀/形状调整后即可对应 | `user/src/api/charging.js:51` |
| POST | `/user/wallet/recharges` | `POST /wallet/top-up` | FRONTEND_CHANGE | 命名 recharges → top-up | `user/src/api/charging.js:56` |
| GET | `/user/wallet/transactions` | `GET /wallet/transactions` | FRONTEND_CHANGE | 前缀/形状调整后即可对应 | `user/src/api/charging.js:61` |

### 7.2 管理端

| 方法 | 前端路径 | Go 对应 | 分类 | 依据 | 调用处 |
| ---- | -------- | ------- | ---- | ---- | ------ |
| GET | `/admin/accounts` | **无** | BACKEND_CHANGE | 无管理员账号管理接口 | `admin/src/api/account.js:7` |
| POST | `/admin/accounts` | **无** | BACKEND_CHANGE | 同上 | `admin/src/api/account.js:12` |
| PUT | `/admin/accounts/{adminId}/status` | **无** | BACKEND_CHANGE | 同上 | `admin/src/api/account.js:17` |
| GET | `/admin/audit-logs` | `GET /admin/audit` | FRONTEND_CHANGE | 命名 + 分页字段 | `admin/src/api/ops.js:7` |
| POST | `/admin/auth/login` | `POST /auth/admin/login` | FRONTEND_CHANGE | 段序不同 | `admin/src/api/auth.js:7` |
| POST | `/admin/auth/logout` | `POST /auth/logout` | FRONTEND_CHANGE | 同上 | `admin/src/api/auth.js:17` |
| POST | `/admin/auth/reauth` | **无** | BACKEND_CHANGE | 无二次认证接口 | `admin/src/api/auth.js:12` |
| GET | `/admin/backups` | **无** | BACKEND_CHANGE | 备份由运维脚本/systemd 承担，未做成 API；需裁定是否暴露给管理端 | `admin/src/api/ops.js:12` |
| POST | `/admin/backups` | **无** | BACKEND_CHANGE | 同上 | `admin/src/api/ops.js:17` |
| POST | `/admin/backups/{backupNo}/verifications` | **无** | BACKEND_CHANGE | 同上 | `admin/src/api/ops.js:22` |
| GET | `/admin/chargers` | `GET /admin/chargers` | FRONTEND_CHANGE | 路径一致；仅分页字段（items+meta）与字段形状需对齐 | `admin/src/api/charger.js:7` |
| POST | `/admin/chargers/{chargerId}/restart-commands` | `POST /admin/chargers/{chargerId}/restart` | FRONTEND_CHANGE | 命令式资源改为动作端点 | `admin/src/api/charger.js:26` |
| PUT | `/admin/chargers/{chargerId}/status` | **无** | BACKEND_CHANGE | 无桩状态变更接口 | `admin/src/api/charger.js:17` |
| POST | `/admin/chargers/batch` | **无** | BACKEND_CHANGE | 无批量建桩接口 | `admin/src/api/charger.js:12` |
| GET | `/admin/device-commands/{commandNo}` | **无** | BACKEND_CHANGE | 无命令状态查询接口 | `admin/src/api/charger.js:35` |
| GET | `/admin/flows` | `GET /admin/orders` | FRONTEND_CHANGE | flow 列表映射为订单列表 | `admin/src/api/flow.js:7` |
| POST | `/admin/flows/{flowNo}/force-releases` | `POST /admin/chargers/{chargerId}/release` | FRONTEND_CHANGE | 强释放按桩而非按流程 | `admin/src/api/flow.js:12` |
| PUT | `/admin/me/password` | **无** | BACKEND_CHANGE | 无管理员改密接口 | `admin/src/api/auth.js:22` |
| POST | `/admin/ml-tasks` | **无** | BLOCKED | ML 属 A-07 | `admin/src/api/ml.js:24` |
| GET | `/admin/ml-tasks/{taskNo}` | **无** | BLOCKED | 同上 | `admin/src/api/ml.js:29` |
| GET | `/admin/predictions` | **无** | BLOCKED | 同上 | `admin/src/api/ml.js:17` |
| POST | `/admin/price-adjustments` | **无** | BACKEND_CHANGE | 无调价单接口 | `admin/src/api/station.js:45` |
| GET | `/admin/stations` | `GET /admin/stations` | FRONTEND_CHANGE | 路径一致；仅分页字段（items+meta）与字段形状需对齐 | `admin/src/api/station.js:7` |
| POST | `/admin/stations` | `POST /admin/stations` | FRONTEND_CHANGE | 前缀/形状调整后即可对应 | `admin/src/api/station.js:12` |
| PUT | `/admin/stations/{stationId}` | **无** | BACKEND_CHANGE | 无站点更新接口 | `admin/src/api/station.js:17` |
| POST | `/admin/stations/{stationId}/{action}` | **无** | BACKEND_CHANGE | 站点启停动作无接口 | `admin/src/api/station.js:26` |
| GET | `/admin/stats/charger-status` | **无** | BLOCKED | 同上 | `admin/src/api/stats.js:68` |
| GET | `/admin/stats/revenue` | **无** | BLOCKED | 统计属 A-07 | `admin/src/api/stats.js:39` |
| GET | `/admin/tariffs` | `GET /admin/chargers/{chargerId}/tariff` | FRONTEND_CHANGE | 全局费率表 → 按桩费率 | `admin/src/api/station.js:35` |
| POST | `/admin/tariffs` | `PUT /admin/chargers/{chargerId}/tariff` | FRONTEND_CHANGE | 同上 | `admin/src/api/station.js:40` |
| GET | `/admin/users` | `GET /admin/users` | FRONTEND_CHANGE | 路径一致；仅分页字段（items+meta）与字段形状需对齐 | `admin/src/api/user.js:10` |
| GET | `/admin/users/{userId}` | `GET /admin/users/{userId}` | FRONTEND_CHANGE | 路径一致；仅分页字段（items+meta）与字段形状需对齐 | `admin/src/api/user.js:15` |
| GET | `/admin/users/{userId}/orders` | **无** | BACKEND_CHANGE | `AdminOrderFilter` 无 UserID（internal/admin/service.go:106-111） | `admin/src/api/user.js:29` |
| PUT | `/admin/users/{userId}/status` | `POST .../freeze` \| `POST .../unfreeze` | FRONTEND_CHANGE | 状态 PUT 改为两个动作端点 | `admin/src/api/user.js:20` |

### 7.3 另有两条需单独记录

- `apps/user/src/api/auth.js:49` 直接拼接 `${API_BASE}/user/me/avatar/content` 作为头像 `<img>` 地址（未走 `request()`）：Go 无此接口 → `BACKEND_CHANGE`。
- `apps/user/src/api/http.js`、`apps/admin/src/api/http.js` 内的 `api.get/post/put/delete` 为请求层自身便捷方法，非业务调用，不计入分类。

### 7.4 对第 2 阶段（契约冻结）的结论

| 结论 | 内容 |
| ---- | ---- |
| MATCH | **0 条**（路径级）；仅传输约定一致：`API_BASE=/api/v1`、Bearer、`X-Request-ID`、`Idempotency-Key`、`{success,code,message,data}` 信封 |
| FRONTEND_CHANGE | **38 条**：路径前缀/段序/命名改动 + 统一改分页解析（`data.total` → `data.meta.total`）|
| BACKEND_CHANGE | **19 条**：Go 无对应能力，需先登记 OpenAPI 再实现（注册、头像上传、进度、按用户查订单、站点更新/启停、批量建桩、桩状态、命令查询、调价单、备份、管理端账号/改密/二次认证、报价等）|
| BLOCKED | **7 条**：Agent chat、导航路线、ML 任务/预测、统计（A-07/A-05 范围）|

**给 A 线的改造优先级建议**（按任务文档第 6 节验收场景顺序）：认证与会话 → 站点/桩 → 钱包（含 `recharges`→`top-up`）→ 订单（含 flow→order 映射）→ 评价/申诉 → 管理端列表与详情 → 其余 BACKEND_CHANGE 项排期。

## 8. 裁定后的执行口径（v2，本轮冻结）

裁定原则：**最少可用需求、轻量稳定、先完成 Go 后端闭环**。以下为逐项落地口径与实施状态。

### 8.1 纳入本轮的 6 类后端接口

| # | 接口 | 现状核对 | 实施状态 |
| - | ---- | -------- | -------- |
| 1 | 用户注册 | Go 只有短信登录隐式建号（`internal/auth/service.go` 无 `Register`） | **待实现**：登记 `POST /auth/user/register` → 实现 → 测试 |
| 2 | 用户订单列表 | `ListFilter` 已有 `CreatedFrom/CreatedTo`，但 handler 只解析 `status`，且无 `sort`；OpenAPI 已登记 `createdFrom/createdTo` | **已完成**（见 8.2） |
| 3 | 头像 URL 读取与更新 | `user_accounts.avatar_url`（0008，空串=无头像）、`GET /me/profile`、`PUT /me/profile` 均已具备 | **已具备**，仅需在契约记录中标注（上传接口不实现，前端改用 URL 字段） |
| 4 | 管理端按用户查询订单 | `AdminOrderFilter` 无 `UserID`，SQL 不支持 | **已完成**（见 8.2） |
| 5 | 命令/订单状态查询 | 订单状态查询已具备（`GET /orders/{orderNo}`、`GET /admin/orders`）；**命令状态查询缺失**（`charger_command_outcomes` 表已有 command_id/charger_id/order_no/action/result/applied/recorded_at） | **待实现**：登记并实现 `GET /admin/device-commands/{commandNo}` |
| 6 | 站点/充电桩状态管理 | `stations.status`、`chargers.status` 列已存在；无状态变更接口 | **待实现**：登记并实现站点/充电桩状态变更端点 |

### 8.2 本轮已完成（含原始验证）

**用户订单列表参数（item 2）** —— 契约登记的是 `createdFrom`/`createdTo`/`sort`，而 handler 此前只解析 `status`（登记了却没实现）。

- `internal/order/http.go`：解析 `createdFrom`/`createdTo`（RFC3339，解析失败 → 400 而不是静默忽略）与 `sort`；
- `internal/order/types.go`：新增 `Sort` 字段与 `SortCreatedAtAsc/SortCreatedAtDesc` 常量，服务层校验 `sort` 取值与 `createdFrom < createdTo`（新增 `ErrInvalidSort`、`ErrInvalidTimeWindow`，映射 400）；
- `internal/repository/postgres/orders.go`：`orderByClause()` 把 `sort` 翻成 `created_at ASC/DESC, id ASC/DESC`，并再次限定取值集合，绝不把调用方文本拼进 SQL；
- `api/openapi.yaml`：`GET /orders` 增补 `sort`（enum `createdAt` / `-createdAt`，默认 `-createdAt`）。

验证：`TestListAcceptsTheRegisteredQuerySurface`（handler 层：参数真的到达服务层；非法日期、`from >= to`、未登记 sort 三种情况均 400）、`TestOrderListSortAndWindowOnRealDatabase`（真实 PG：默认倒序、`createdAt` 正序、未来/过去窗口返回 0、近一小时窗口返回 2）。

**管理端按用户查订单（item 4）**

- `internal/admin/service.go`：`AdminOrderFilter` 增 `UserID`（0 = 不过滤）；
- `internal/repository/postgres/admin.go`：过滤条件 `AND ($3 = 0 OR user_id = $3)`；
- `internal/admin/http.go`：解析 `userId`，非正整数或非数字 → 400；
- `api/openapi.yaml`：`GET /admin/orders` 增补 `userId`（integer/int64，minimum 1）。

验证：`TestAdminOrderListFiltersByUser`（handler 层：`userId=42` 到达服务层；`0/-3/abc/4.2` 均 400；缺省时 UserID=0 仍列出全部）、`TestAdminUserAndOrderLists`（真实 PG：两个用户各一单，按用户过滤只返回该用户的单，total=1）。

实现中修掉的一处真实缺陷：把用户过滤写成 `$5`、而 `$3/$4` 是 LIMIT/OFFSET 时，**计数查询引用了未参与类型的占位符**，PostgreSQL 直接报 `42P18 could not determine data type of parameter $3`；改为「过滤参数在前、分页参数在后」，计数查询使用同一参数列表的连续前缀。

**反向验证**（移除行为后测试必须失败，均已实测）：

| 回退动作 | 观察到的失败 |
| -------- | ------------ |
| handler 不再解析 `createdFrom/createdTo` | `http_test.go: createdFrom = <nil>, want 2026-01-01 00:00:00 +0000 UTC` |
| 仓储忽略 `sort`（恒为倒序） | `orders_integration_test.go: ascending first item = ORD…fc6ef2cc, want the oldest ORD…cabc3dd4` |
| 管理端不做按用户过滤 | `admin_integration_test.go: filtered orders = [...整张表...], want exactly ORD… for user 70` |

质量门禁：`gofmt` 干净、`go build ./...`、`go vet ./...`、`go test -count=1 -race ./...`（17 个包 ok）。

### 8.3 前端改造 / 删除 / 延期（与裁定一致）

| 项 | 口径 |
| -- | ---- |
| 实时进度 | 前端用订单详情轮询，**不引入 WebSocket**；后端不新增进度端点 |
| 站点报价 | 使用现有费率与订单金额推导，**不新增报价接口** |
| 调价单 | 使用现有按桩费率更新，**不新增调价单模型** |
| 批量建桩 | 前端**删除入口** |
| 备份 API | 继续用部署脚本，**不开放业务 API** |
| 管理端账号、改密、二次认证 | 本轮**删除入口** |
| 独立 ML/统计任务 | 归入 A-07，保持 `BLOCKED` |
| `flow` 模型 | 采用 Go 订单状态机：前端删除"报价确认""手动结算"两个独立步骤，START/STOP 由设备回执推进，STOP 成功后后端自动结算；**不新增 C++ 风格 flow 接口** |
| 费率模型 | 采用按充电桩费率 `/admin/chargers/{chargerId}/tariff`；原 `/admin/tariffs` 全局费率页面改为按桩维护；**不新增全局费率表、不新增迁移** |

### 8.4 两线执行顺序

A 线先完成：① 去除 `/user` 前缀与旧登录路径；② 统一分页读 `data.meta.total`；③ 统一 Bearer/幂等键/envelope；④ 删除不纳入本轮的旧 C++ 页面入口；⑤ 接入现有 Go 用户、站点、钱包、订单与管理接口。
B 线随后登记并实现：① 注册；② 用户订单列表（**已完成**）；③ 头像 URL（**已具备**）；④ 管理端按用户查询订单（**已完成**）；⑤ 命令/订单状态查询；⑥ 必要的站点与充电桩状态管理接口。

**当前结论：前端不能立即进行完整联调，先完成全局前端适配；B 线只补上述 6 类接口，其余能力明确删除或延期 A-07。**

## 9. 第二批实施记录（注册 / 命令状态查询 / 站点与充电桩状态变更）

三项均按"先登记 OpenAPI，再实现，最后补测试"的顺序完成，并各自做了反向验证。OpenAPI 操作数由 45 增至 **49**。

### 9.1 注册 `POST /api/v1/auth/user/register`

- Go 此前只有短信登录隐式建号（`EnsureUserWithWallet` 用 `ON CONFLICT DO NOTHING` 覆盖语义），**没有注册**。
- 新增：`internal/auth` 的 `Register(ctx, username, phone, password, code)` + `ErrAccountExists` 哨兵；`AccountWriter.RegisterUser`（新接口方法）→ `internal/repository/postgres/accounts.go` 的 `RegisterUser`（同一事务建号 + 建零余额钱包，唯一索引冲突映射为业务错误）。
- **校验顺序是刻意的**：先校验手机号与密码格式（调用方自己的输入）→ 再校验短信验证码（手机归属的证明）→ 最后才建号。先建号会让每次输错都留下半注册账号；先验码会让任何人用格式错误的请求烧掉别人手机号的验证码。
- 密码规则复用现有 `HashPassword`（8..128，`ErrPasswordLength` → 400）；验证码复用现有 `SMSCodeStore.Verify`（单次有效、失败计数上限）。
- 重复账号 → **409 / code 5 `ALREADY_EXISTS`**（错误码注册表 `docs/database-api.md` 已有该码，未新增码、未新增迁移）。重复判定发生在验码之后，因此**没有有效验证码就无法探测某手机号是否已注册**——这一点写进了 OpenAPI 描述。
- 注册成功返回 **201 + 会话**（`LoginResponseEnvelope`，含 accessToken），前端无需再登录一次。
- 验证：`TestRegisterEndpoint`（错误验证码 → 422/20 且**未建号**；弱密码 → 400 且未建号；成功 → 201 + 会话 + 存储哈希可用明文校验通过；重复 → 409/5；缺手机号 → 400 且未触达写库）、`TestRegisterUserOnRealDatabase`（真实 PG：建号 + 空钱包、哈希可验、重复 → `ErrAccountExists` 且用户/钱包各 1 行、被拒请求未改名）。
- 反向验证：去掉验码 → `wrong code: status = 201 code = 0`（**错误验证码也能注册成功**）；去掉唯一冲突映射 → `duplicate registration error = ERROR: duplicate key value violates unique constraint "user_accounts_phone_key" (SQLSTATE 23505), want ErrAccountExists`；去掉建钱包 → `registered user has no wallet`。

### 9.2 命令状态查询 `GET /api/v1/admin/device-commands/{commandId}`

- 路径参数使用 **`commandId`**，与数据库 `charger_command_outcomes.command_id` 同名同义；返回字段 `commandId/chargerId/orderNo/action/result/applied/recordedAt`。
- **顺带消掉的命名不一致**：既有 `Command{commandNo,status}`（`POST /admin/chargers/{chargerId}/restart` 的 202 响应）用的也是同一个标识符，却叫 `commandNo`。已统一为 `commandId`（Go 结构、OpenAPI schema、审计载荷键、集成测试断言同步）。**前端需把重启响应与轮询里的 `commandNo` 改为 `commandId`**（`apps/admin/src/stores/chargers.js` 3 处、`apps/admin/src/api/charger.js` 1 处参数名）。
- 权限：读取属管理员只读范围（`RequireRole(RoleAdmin)`，AUDITOR 可读、普通用户 401/403）；未找到 → **404 / code 4**。
- 数据来源：真实回执路径写入的 `charger_command_outcomes`（不是队列状态），因此"命令已下发但设备未应答"就是 404，不伪造 PENDING。
- 验证：`TestGetDeviceCommand`（200 且按 id 查、响应键为 `commandId` 而非 `commandNo`、404、普通用户被拒）、`TestAdminDeviceCommandLookupOnRealDatabase`（真实 PG：通过 `RecordChargerCommandResult` 写回执后查得 action/result/applied/recordedAt=RFC3339；未知 id → `ErrDeviceCommandNotFound`）。
- 反向验证：去掉 404 映射 → `missing command: status = 503 code = 3`；仓储吞掉 `ErrNoRows` → `unknown command error = <nil>, want ErrDeviceCommandNotFound`（静默返回空记录）。

### 9.3 站点 / 充电桩状态变更

`PUT /api/v1/admin/stations/{stationId}/status` 与 `PUT /api/v1/admin/chargers/{chargerId}/status`，body `{"status": "..."}`。

- **状态枚举严格校验**（站点 `OPEN/CLOSED/DISABLED`、充电桩 `IDLE/OCCUPIED/FAULT/RESTARTING/DISABLED`），未登记值 → 400 且**不触达存储**。
- **转换合法性**由新文件 `internal/station/lifecycle.go` 统一定义并在事务内、行锁下校验：
  - 站点：`OPEN⇄CLOSED`、`OPEN→DISABLED`、`CLOSED→DISABLED`、`DISABLED→OPEN`；**拒绝** `DISABLED→CLOSED`（必须先回到 OPEN，运维动作显式化）与"改成当前状态"（409，而非静默成功）。
  - 充电桩：`IDLE⇄DISABLED`、`FAULT→IDLE/DISABLED`；**拒绝** `OCCUPIED→*`（必须先强制释放，否则活动订单会占着一个已不可用的桩）与 `RESTARTING→*`（重启命令在途，落点由回执决定）。
- **审计**：同事务写 `operation_logs`，action `station.status` / `charger.status`，payload `{"from":..., "to":...}`；被拒的转换**不产生**审计行。
- **权限**：两个端点都用 `RequireAdminWrite`，AUDITOR（只读管理员）与普通用户 → 403。
- 充电桩更新同时 `version = version + 1`；站点无 version 列，只更新 `updated_at`。**未新增迁移**。
- 验证：`TestChangeStationStatus` / `TestChangeChargerStatus`（200、命令到达存储、未登记状态 400 且未触达存储、非法转换 409、未知 id 404、AUDITOR 403、GET 405）、`TestAdminStatusChangesOnRealDatabase`（真实 PG：合法转换生效且审计各 1 行、`DISABLED→CLOSED` 与"同状态"被拒且行不变、`DISABLED→OPEN` 可回、充电桩 version +1、`OCCUPIED→DISABLED` 被拒、未知 id 报 `ErrStationNotFound`/`ErrChargerNotFound`、被拒转换不产生审计行）。
- 反向验证：去掉转换校验 → `DISABLED -> CLOSED error = <nil>, want ErrInvalidStateTransition`；去掉审计写入 → `station audit rows = 0, want 1`；把 `RequireAdminWrite` 换回 `RequireRole` → `auditor write = 200, want 403`。

### 9.4 实施中顺带发现的既有缺陷

**D-5 `StationRecord` 没有 JSON tag（已修，并附一次勘误）**：状态变更端点此前返回 Go 风格键（`ID`/`Code`/`Status`），而登记的是 camelCase 的 `Station` schema；新测试第一次就撞上了它。已补 tag（`id/code/name/address/latitudeE6/longitudeE6/status`）并加 `TestCreateStationResponseUsesContractKeys` 防回归。

**勘误**：上一轮报告里"`POST /admin/stations` 的响应不含 `chargerCount/idleChargerCount/minPriceCentPerKwh` 三个聚合字段"是**错的**。核对 `internal/admin/http.go` 的 `createStation` 后确认：创建响应是显式构造的 map，**包含全部 10 个 `Station` 字段**，三个聚合被写成 0——对新站而言这是**真实值**（0 个桩、0 个空闲、无价格），不是占位符。真正有 Go 风格键问题的只有状态变更端点。据此按二审口径实现见第 10.2 节。

### 9.5 第二批质量门禁

`gofmt` 干净、`go build ./...`、`go vet ./...`、`go test -count=1 -race ./...`（见提交信息中的包数），OpenAPI YAML 可解析且操作数 49。

## 10. 第二批收口（二审要求的两项 + 验证码口径）

### 10.1 收口项一：前端 `commandNo` → `commandId`（A 线文件，本线只产出补丁）

改动落在 fork 分支 `feat/postgres-agent-web-migration`（`apps/*` 属 A 线，B 线不改这些文件），补丁见 `pr42-command-id-rename.patch`。

**必须改 4 个文件，不是二审点名的 2 个**（只改那 2 个会留下两处坏点）：

| 文件 | 处数 | 不改的后果 |
| ---- | ---- | ---------- |
| `apps/admin/src/api/charger.js` | 2 | 轮询参数名与后端 `{commandId}` 不匹配 |
| `apps/admin/src/stores/chargers.js` | 7 | 重启响应读不到标识 → 轮询不启动 |
| `apps/admin/src/views/ChargersView.vue` | 1 | 页面显示"命令编号 "（空值） |
| `apps/admin/tests/chargers.test.js` | 8 | 前端测试直接失败 |

替换后静态核对：`grep -rn "commandNo" apps agent` = **0**；两个被改的 ESM 文件 `node --check` 通过；补丁 17 行改动（4 文件）。

**未完成的一步（如实说明）**：本机为验证改名后前端仍可用，在 fork 的临时克隆里执行了 `npm ci`（admin 有 lockfile，registry 可达），但安装未在本次窗口内完成（node v18 低于包要求的 20+，仅 EBADENGINE 警告），因此**前端 `vitest` 尚未跑过**。补丁是纯标识符重命名（无逻辑改动），静态检查已通过；建议 A 线应用后跑一次 `npm test`。

### 10.2 收口项二：`POST /admin/stations` 响应契约

按二审建议采用**独立 schema**，但依据勘误后的实施事实做了精确处理：

- 新增 `StationCreateResponse`：**required 只含写操作真正产出的 7 个字段**（`id/code/name/address/status/latitudeE6/longitudeE6`）；三个聚合字段**声明为可选属性**并注明"写操作可不计算，读端点才计算"。
- 创建接口的 201 响应改登记该 schema（信封 `StationCreateResponseEnvelope`）；状态变更端点复用同一 schema，删除了重复的 `StationStatusRecord` 定义。
- **响应体未改**：创建仍返回 10 个字段（三个聚合为 0，对新站是真实值），只是 schema 不再**要求**它们——这样将来创建接口不再计算聚合也依然合规，符合二审"避免创建接口额外查询"的意图，也不会让前端表格里少字段。
- 新增 `backend/internal/admin/schema_contract_test.go`：把响应键与 schema 双向对拍（响应的键必须恰好等于契约命名的键；schema 的 `required` 列表必须与该列表一致），覆盖创建、站点状态、充电桩状态、命令查询四处。**不引入任何新依赖**（用标准库定点抽取 schema 的 `required` 列表；若该列表被改格式，测试会明确报错而不是静默通过）——这正是能抓出 D-5 那一类问题的检查。
- 反向验证：把 `chargerCount` 写回 `required` → `StationCreateResponse requires [address chargerCount code id ...], want [address code id ...]`。

### 10.3 验证码 `purpose` 口径（本轮裁定：继续复用手机号验证码）

在 OpenAPI 里明确写出，避免前端误以为已实现用途隔离：

- `/auth/user/sms/code` 新增 description：验证码按手机号存储，**不按 `purpose` 隔离**；请求接受 `LOGIN/REGISTER/RESET_PASSWORD` 并返回成功，但发出的码对三者通用；参数保留在契约中以便客户端今天就传，**用途隔离是未实现的行为变更**，客户端不得依赖"非本用途的码会被拒"。
- `/auth/user/register` 的描述里指向该说明（注册用的是手机号的码，不是注册专用码）。

