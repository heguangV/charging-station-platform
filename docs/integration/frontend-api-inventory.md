# 前端接口盘点（来源：PR #42 fork 分支）

## 0. 来源与口径

- 仓库：`https://github.com/cjyhjy/charging-station-platform`（fork）
- 分支：`feat/postgres-agent-web-migration`
- 本次盘点所用提交：`4d1a265 fix(postgres): align with v10 order confirmation schema`（`git clone --depth 1 -b feat/postgres-agent-web-migration`，**独立临时克隆，未改动后端仓库，也未改动任何前端文件**）
- 盘点口径：只读扫描 `apps/user/`、`apps/admin/`、`apps/shared/`、`agent/` 的源码与配置；逐条记录调用处、方法、路径、幂等标记，并与 `develop` 的 Go 路由 / `api/openapi.yaml` 对照。
- 归属说明：`frontend-api-inventory.md` 按任务文档第 4 节属 A 线交付物。本次由 B 线**基于 fork 实际代码**生成，供 A 线确认或修订；B 线的映射结论以 `frontend-go-api-matrix.md` 为准。

## 1. 前端结构与请求层

| 位置 | 内容 |
| ---- | ---- |
| `apps/user/src/api/` | `http.js`（请求层）、`auth.js`、`station.js`、`order.js`、`charging.js`、`agent.js` |
| `apps/admin/src/api/` | `http.js`、`auth.js`、`user.js`、`account.js`、`charger.js`、`station.js`、`flow.js`、`ops.js`、`stats.js`、`ml.js` |
| `apps/shared/` | 无 TS/JS 源码（0 个文件） |
| `agent/` | C++ 工程（`CMakeLists.txt`、`include/`、`src/`），**无 Web/TS 源码**；前端对 Agent 的调用在 `apps/user/src/api/agent.js` |

请求层关键事实（`apps/user/src/api/http.js`、`apps/admin/src/api/http.js`）：

- `API_BASE = '/api/v1'`，所有路径由此拼接；
- `request(path, options)` 统一实现：`Authorization: Bearer <accessToken>`、`X-Request-ID`（客户端随机生成）、`Content-Type: application/json`、`Idempotency-Key`（`idempotent: true` 时生成或复用）、8s 默认超时（Agent 20s）、`cache: 'no-store'`；
- 解包 `{success, code, message, userMessage, requestId, data}`，`success !== true || code !== 0` 抛 `ApiError`；`ApiErrorKind` 只有 `NETWORK/TIMEOUT/ABORTED/INVALID_RESPONSE/HTTP`（**不解析 Go 的业务码**）；
- **组件内不存在绕过请求层的 `fetch`/`axios`**（`grep -rln "fetch(\|axios" apps/user/src apps/admin/src agent` 在 `src/api/` 之外命中 0 个文件）→ 本清单即完整请求面；
- dev 代理：`vite.config.js` 把 `/api` 转发到 `VITE_NCS_API_TARGET`（默认 `https://127.0.0.1:8443`，即 Nginx HTTPS 入口），**无路径重写**；
- 运行时依赖：`vue`/`pinia`/`vue-router`（admin 另加 `echarts`）——**无 `pg`/`sqlite`/`mysql`/`axios`/ORM 依赖**；
- 环境变量白名单 `envPrefix: ['VITE_', 'TENCENT_MAP_JS_KEY']`，服务端专用 key（`AI_*`、`TENCENT_MAP_SERVER_KEY`）不进前端产物。

## 2. 与 Go 后端的全局差异（先看这四条，再看逐条矩阵）

1. **路径前缀系统性不一致**：前端用 `/user/...`（如 `/user/me`、`/user/orders`），Go 用 `/me`、`/orders`；登录类前面顺序也不同（前端 `/user/auth/login/password` ↔ Go `/auth/user/login`）。
2. **分页字段不一致**：前端读 `data.items` + `data.total`；Go 返回 `data.items` + `data.meta{page,pageSize,total}`（`internal/order/types.go:102-103` 等）。**所有列表接口都受影响**。
3. **信封多两个字段**：前端期望 `userMessage` 与 `requestId`；Go 信封是 `{success, code, message, data}`。前端对缺失是容错的（`userMessage` 回退到 `message`，`requestId` 用本地随机值），因此不构成阻塞，但提示文案会退化。
4. **业务码未使用**：前端只按 HTTP 状态分流；Go 的 `code`（1/3/4/6/14/18/19）未被前端使用。

> 关键证据：同一 fork 的 `docs/api-integration.md` 开头即写明"本文档与当前 Go 后端实现保持一致……不要根据旧 C++ 接口自行推断路径或字段"，其接口表列出的路径正是 Go 的路径。**前端代码尚未按该文档迁移**，仍使用旧契约路径。这正是本次联调要收敛的部分。

## 3. 用户端调用清单（31 条）

| 方法 | 前端路径 | 调用处（文件:行） | 函数 | 幂等 |
| ---- | -------- | ----------------- | ---- | ---- |
| GET | `${path}${buildQuery(params)}` | `user/src/api/http.js:255` | `api` |  |
| POST | `/user/agent/chat` | `user/src/api/agent.js:16` | `chatWithAgent` |  |
| POST | `/user/auth/login/password` | `user/src/api/auth.js:17` | `loginWithPassword` |  |
| POST | `/user/auth/login/sms` | `user/src/api/auth.js:12` | `loginWithSms` |  |
| POST | `/user/auth/logout` | `user/src/api/auth.js:27` | `logout` |  |
| POST | `/user/auth/register` | `user/src/api/auth.js:22` | `registerAccount` |  |
| POST | `/user/auth/sms/code` | `user/src/api/auth.js:7` | `sendSmsCode` |  |
| POST | `/user/flows` | `user/src/api/charging.js:7` | `requestFlow` | yes |
| GET | `/user/flows/${encodeURIComponent(flowNo)}` | `user/src/api/charging.js:17` | `fetchFlow` |  |
| POST | `/user/flows/${encodeURIComponent(flowNo)}/cancellations` | `user/src/api/charging.js:27` | `cancelFlow` | yes |
| GET | `/user/flows/${encodeURIComponent(flowNo)}/progress` | `user/src/api/charging.js:41` | `fetchProgress` |  |
| POST | `/user/flows/${encodeURIComponent(flowNo)}/quote-confirmations` | `user/src/api/charging.js:22` | `confirmQuote` | yes |
| POST | `/user/flows/${encodeURIComponent(flowNo)}/settlements` | `user/src/api/charging.js:46` | `settleFlow` | yes |
| POST | `/user/flows/${encodeURIComponent(flowNo)}/start` | `user/src/api/charging.js:32` | `startFlow` | yes |
| GET | `/user/flows/active` | `user/src/api/charging.js:12` | `fetchActiveFlow` |  |
| GET | `/user/me` | `user/src/api/auth.js:32` | `fetchProfile` |  |
| PUT | `/user/me` | `user/src/api/auth.js:37` | `updateNickname` |  |
| POST | `/user/me/avatar` | `user/src/api/auth.js:44` | `uploadAvatar` | yes |
| GET | `/user/orders` | `user/src/api/order.js:7` | `fetchOrders` |  |
| GET | `/user/orders/${encodeURIComponent(orderNo)}` | `user/src/api/order.js:12` | `fetchOrderReceipt` |  |
| GET | `/user/orders/${encodeURIComponent(orderNo)}/review` | `user/src/api/order.js:17` | `fetchOrderReview` |  |
| POST | `/user/orders/${encodeURIComponent(orderNo)}/review` | `user/src/api/order.js:22` | `submitOrderReview` | yes |
| GET | `/user/stations` | `user/src/api/station.js:11` | `fetchStations` |  |
| GET | `/user/stations/${encodeURIComponent(stationId)}` | `user/src/api/station.js:23` | `fetchStation` |  |
| GET | `/user/stations/${encodeURIComponent(stationId)}/chargers` | `user/src/api/station.js:28` | `fetchChargers` |  |
| GET | `/user/stations/${encodeURIComponent(stationId)}/quote` | `user/src/api/station.js:33` | `fetchQuote` |  |
| GET | `/user/stations/${encodeURIComponent(stationId)}/reviews` | `user/src/api/station.js:53` | `fetchStationReviews` |  |
| GET | `/user/stations/${encodeURIComponent(stationId)}/route` | `user/src/api/station.js:42` | `fetchRoute` |  |
| GET | `/user/wallet` | `user/src/api/charging.js:51` | `fetchWallet` |  |
| POST | `/user/wallet/recharges` | `user/src/api/charging.js:56` | `rechargeWallet` | yes |
| GET | `/user/wallet/transactions` | `user/src/api/charging.js:61` | `fetchWalletTransactions` |  |

## 4. 管理端调用清单（35 条）

| 方法 | 前端路径 | 调用处（文件:行） | 函数 | 幂等 |
| ---- | -------- | ----------------- | ---- | ---- |
| GET | `${path}${buildQuery(params)}` | `admin/src/api/http.js:316` | `api` |  |
| GET | `/admin/accounts` | `admin/src/api/account.js:7` | `fetchAccounts` |  |
| POST | `/admin/accounts` | `admin/src/api/account.js:12` | `createAccount` | yes |
| PUT | `/admin/accounts/${encodeURIComponent(adminId)}/status` | `admin/src/api/account.js:17` | `setAccountStatus` | yes |
| GET | `/admin/audit-logs` | `admin/src/api/ops.js:7` | `fetchAuditLogs` |  |
| POST | `/admin/auth/login` | `admin/src/api/auth.js:7` | `login` |  |
| POST | `/admin/auth/logout` | `admin/src/api/auth.js:17` | `logout` |  |
| POST | `/admin/auth/reauth` | `admin/src/api/auth.js:12` | `reauth` |  |
| GET | `/admin/backups` | `admin/src/api/ops.js:12` | `fetchBackups` |  |
| POST | `/admin/backups` | `admin/src/api/ops.js:17` | `createBackup` | yes |
| POST | `/admin/backups/${encodeURIComponent(backupNo)}/verifications` | `admin/src/api/ops.js:22` | `verifyBackup` | yes |
| GET | `/admin/chargers` | `admin/src/api/charger.js:7` | `fetchChargers` |  |
| POST | `/admin/chargers/${encodeURIComponent(chargerId)}/restart-commands` | `admin/src/api/charger.js:26` | `createRestartCommand` | yes |
| PUT | `/admin/chargers/${encodeURIComponent(chargerId)}/status` | `admin/src/api/charger.js:17` | `setChargerStatus` | yes |
| POST | `/admin/chargers/batch` | `admin/src/api/charger.js:12` | `createChargersBatch` | yes |
| GET | `/admin/device-commands/${encodeURIComponent(commandNo)}` | `admin/src/api/charger.js:35` | `fetchDeviceCommand` |  |
| GET | `/admin/flows` | `admin/src/api/flow.js:7` | `fetchFlows` |  |
| POST | `/admin/flows/${encodeURIComponent(flowNo)}/force-releases` | `admin/src/api/flow.js:12` | `forceReleaseFlow` | yes |
| PUT | `/admin/me/password` | `admin/src/api/auth.js:22` | `changeOwnPassword` |  |
| POST | `/admin/ml-tasks` | `admin/src/api/ml.js:24` | `startMlTask` | yes |
| GET | `/admin/ml-tasks/${encodeURIComponent(taskNo)}` | `admin/src/api/ml.js:29` | `fetchMlTask` |  |
| GET | `/admin/predictions` | `admin/src/api/ml.js:17` | `fetchPredictions` |  |
| POST | `/admin/price-adjustments` | `admin/src/api/station.js:45` | `createPriceAdjustment` | yes |
| GET | `/admin/stations` | `admin/src/api/station.js:7` | `fetchStations` |  |
| POST | `/admin/stations` | `admin/src/api/station.js:12` | `createStation` | yes |
| PUT | `/admin/stations/${encodeURIComponent(stationId)}` | `admin/src/api/station.js:17` | `updateStation` | yes |
| POST | `/admin/stations/${encodeURIComponent(stationId)}/${action}` | `admin/src/api/station.js:26` | `setStationEnabled` | yes |
| GET | `/admin/stats/charger-status` | `admin/src/api/stats.js:68` | `fetchChargerStatusStats` |  |
| GET | `/admin/stats/revenue` | `admin/src/api/stats.js:39` | `fetchRevenueStats` |  |
| GET | `/admin/tariffs` | `admin/src/api/station.js:35` | `fetchTariffs` |  |
| POST | `/admin/tariffs` | `admin/src/api/station.js:40` | `createTariff` | yes |
| GET | `/admin/users` | `admin/src/api/user.js:10` | `fetchUsers` |  |
| GET | `/admin/users/${encodeURIComponent(userId)}` | `admin/src/api/user.js:15` | `fetchUser` |  |
| GET | `/admin/users/${encodeURIComponent(userId)}/orders` | `admin/src/api/user.js:29` | `fetchUserOrders` |  |
| PUT | `/admin/users/${encodeURIComponent(userId)}/status` | `admin/src/api/user.js:20` | `setUserStatus` | yes |

说明：`apps/*/src/api/http.js` 内部的 `api.get/post/put/delete` 是请求层自身的便捷方法（第一个参数是变量 `path`，不是具体路径），不计入上表。

## 5. 依赖判定

- **C++/SQLite 运行时依赖：无**（package.json 无相关包，源码中无直连数据库的调用）。
- **旧契约依赖：有**（路径、分页字段），这是当前真正阻碍联调的部分。
- **Agent**：前端只有 `apps/user/src/api/agent.js` → `POST /user/agent/chat`；Go 未实现、未登记 → `BLOCKED`。
- **C++ 服务端 PostgreSQL v10 改造**：与本盘点无交集（前端不访问数据库，也不直连 C++ 端口），按任务文档**不纳入 Go 后端联调**。

## 6. 结论

| 项 | 盘点前 | 盘点后 |
| -- | ------ | ------ |
| 前端 Web | 待核对 | **已核对**：请求面完整（66 条调用、全部经统一请求层），但路径与分页字段仍绑定旧契约 → 需要一份 FRONTEND_CHANGE 清单（见 `frontend-go-api-matrix.md` 第 7 节） |
| Agent | BLOCKED | **BLOCKED（确认）**：Go 无 `/api/v1/user/agent/chat`，前端调 `/user/agent/chat` |
| C++ PG v10 | 不纳入 | **确认不纳入** |
| 前端接口矩阵 | 等待补齐 | **已补齐**（`frontend-go-api-matrix.md` 第 7 节给出逐条分类与依据） |

## 7. 与裁定相关的参数名结论（补充）

盘点时发现订单列表存在**三方不一致**：OpenAPI 登记的是 `createdFrom`/`createdTo`，handler 此前两个都没解析（只解析 `status`），而前端发送的是 `fromAt`/`toAt`。

按裁定"前端适配"处理：**保留已登记的 `createdFrom`/`createdTo`**，B 线把 handler 补齐（并同时支持 `sort`），前端把 `fromAt`/`toAt` 改为 `createdFrom`/`createdTo`。详见 `frontend-go-api-matrix.md` 第 8.2 节。

