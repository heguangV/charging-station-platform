# 前端 HTTP 请求盘点（A-01）

> A 线交付物。对照基线：本分支（`codex/frontend/go-api-adapter`，基于 `origin/pr-42` @ `4d1a265`）；
> Go 契约基线：`develop` @ `422ee83` 的 `api/openapi.yaml`（44 个操作）。
> B 线基于本文产出 `frontend-go-api-matrix.md` 并标记 MATCH / FRONTEND_CHANGE / BACKEND_CHANGE / BLOCKED。

## 0. 盘点方法与证据口径

- 覆盖范围：`apps/user/src`、`apps/admin/src`、`agent/`。对三个目录全量检索了
  `fetch(`、`XMLHttpRequest`、`EventSource`、`WebSocket`、`sendBeacon` 与 `api.get/post/put/delete`
  调用，除本文登记项外**不存在其他网络请求**（证据：检索于 2026-09-16，本分支工作树）。
- 每一行给出调用文件与函数；行号以本分支为准。
- 两个前端已有统一请求层 `apps/user/src/api/http.js` 与 `apps/admin/src/api/http.js`，
  统一注入以下请求头，**下文表格不再逐行重复**，只登记偏离项：
  - `Accept: application/json`、`X-Request-ID`（前端生成 UUID）；
  - `Authorization: Bearer <accessToken>`（有令牌时）；
  - `Content-Type: application/json; charset=utf-8`（有 JSON 请求体时）；
  - `Idempotency-Key`（仅 `idempotent: true` 的业务写入，缺省随机 UUID）。
- 状态分类（对接任务文档 §4 第二阶段前的 A 线初判，最终以 B 线矩阵为准）：
  - **已匹配**：方法、路径一致，语义可直连（body 仍需 B-01 逐字段对照）；
  - **需改造**：Go 存在同语义端点，但路径、参数或字段名需调整；
  - **后端缺失**：Go 无对应能力，需 B-01 决定补契约（BACKEND_CHANGE）或前端改用已有端点（FRONTEND_CHANGE）；
  - **BLOCKED**：依赖 A-07 或外部模块。
- 领域模型术语差异贯穿全部清单：前端旧契约为 **flow（充电流程）** 模型
  （报价确认 → 开始 → 进度 → 结算），Go 契约为 **order（订单）** 模型
  （创建即绑定设备与价格快照 → START/STOP 异步设备命令 → 回执驱动结算）。
  前端旧契约用数字状态码（流程 10/20/30、订单 60/70/90），Go 用字符串枚举
  （CREATED/STARTING/CHARGING/STOPPING/COMPLETED/CANCELLED/EXPIRED/FAILED）。

## 1. 横切差异（影响全部请求，A-02 统一处理）

| # | 差异点 | 前端现状 | Go 现状 | 处置建议 |
|---|---|---|---|---|
| C1 | 信封字段 | 读取 `userMessage`、`requestId`，二者缺失时回退 `message`/自生成 ID（`apps/*/src/api/http.js`） | 信封为 `success/code/message/data` + `traceId`（`backend/internal/httpapi/server.go:58-64`），**无 `userMessage`** | 前端把 `requestId` 回退链改为 `traceId`（一行级改动）；`userMessage` 继续回退 `message` 即可工作 |
| C2 | 业务错误码 | 常量表 0..23（`apps/admin/src/api/http.js:16-43`），含 `REAUTH_REQUIRED=23` | 使用同表子集（0/1/2/3/4/5/6/7/8/15/19…，401/403），**未实现 23** | A-02 收敛前端常量表为 Go 实际子集；23 的降级见 C6 |
| C3 | 分页 | `page/pageSize` + 响应 `{items, meta:{page,pageSize,total}}` | 相同（`httpapi.ParsePagination`，`maxPageSize=100`） | 无需改造 |
| C4 | 度量单位 | 整数分 / 毫瓦时 / 经纬度 E6 | 相同 | 无需改造 |
| C5 | 登录响应 | `session.accessToken` + `session.expiresAt`（`apps/user/src/stores/auth.js:50-54`）；管理端额外消费 `admin.mustChangePassword`（`apps/admin/src/stores/auth.js:75`） | `LoginResponse{accessToken, expiresAt, identity{id, role, displayName, status}}` | token/expiredAt 可直连；`identity` 需前端适配；`mustChangePassword` 后端缺失（见管理端 #4） |
| C6 | 敏感操作重验证 | `/admin/auth/reauth` + 错误码 23 驱动的"重验证后原幂等键重试"完整闭环（`apps/admin/src/stores/auth.js:27-29`） | Go 无 reauth 端点、无错误码 23 | 需 B-01 契约决策：补后端，或 A 线临时降级（隐藏敏感操作入口） |
| C7 | 状态枚举 | 数字码（60/70/90 等） | 字符串枚举 | A-02 建立双向映射表，视图层不改 |
| C8 | 时间过滤参数 | `fromAt/toAt`（钱包流水、订单列表、审计） | Go 均不支持时间范围参数 | 前端去掉或 B 线补参数（B-01 决策） |
| C9 | 未评价查询 | `GET .../review` 期望未评价返回 `{review: null}` | Go 返回 404 | 前端把 404 映射为 `{review: null}`（局部改动） |

## 2. 用户端清单（apps/user/src/api/）

| # | 调用位置 | 方法与路径 | 请求体 | 前端消费的响应 | Go OpenAPI 对应 | 状态 | 差距说明 |
|---|---|---|---|---|---|---|---|
| U1 | `auth.js:6 sendSmsCode` | POST `/user/auth/sms/code` | `{phone, purpose}` | `developmentCode`、`retryAfterSec`（`stores/auth.js:69-70`） | POST `/auth/user/sms/code` | 需改造 | 路径前缀；Go 无 `purpose`；响应为 `{phone, expiresInSec, code?}`（`code` 仅模拟短信），字段名与前端不同 |
| U2 | `auth.js:11 loginWithSms` | POST `/user/auth/login/sms` | `{phone, smsCode, deviceId}` | `accessToken/expiresAt` | POST `/auth/user/login/sms` | 需改造 | 路径前缀；`smsCode→code`；Go 不收 `deviceId`；`identity` 结构适配（C5） |
| U3 | `auth.js:16 loginWithPassword` | POST `/user/auth/login/password` | `{loginName, password, deviceId}` | `accessToken/expiresAt` | POST `/auth/user/login` | 需改造 | 路径；`loginName→account`；`deviceId` |
| U4 | `auth.js:21 registerAccount` | POST `/user/auth/register` | `{phone, password, smsCode, username?}` | 登录会话（201 即建立会话） | POST `/auth/user/register` | 需改造 | B 线已实现（201 返回 LoginResponse，注册即登录）；前端已接线：不提交 deviceId，username 可选 |
| U5 | `auth.js:26 logout` | POST `/user/auth/logout` | — | — | POST `/auth/logout` | 需改造 | 仅路径（Go 为用户/管理员统一端点） |
| U6 | `auth.js:31 fetchProfile` | GET `/user/me` | — | `user`、`balanceCent`、`debtCent`、`hasActiveFlow`、`version`（`stores/auth.js:28-32,113`） | GET `/me` + GET `/me/profile` + GET `/wallet` | 需改造 | Go 无合并资料视图；前端需改为聚合 `/me`、`/me/profile`、`/wallet`，活动流程经 `/orders` 查询 |
| U7 | `auth.js:36 updateNickname` | PUT `/user/me` | `{nickname, version}` | 更新后的 user | PUT `/me/profile` | 需改造 | 路径；`nickname→displayName`；Go 无 `version` 乐观锁 |
| U8 | `ProfileView.vue saveAvatarUrl`（原 `auth.js:41 uploadAvatar`） | PUT `/me/profile` | `{avatarUrl}`（≤512，空串=无头像） | 更新后的资料 | PUT `/me/profile` | 需改造 | Go 契约不提供文件上传，头像为 avatarUrl URL 字段；前端已改为 URL 方式设置与直显 |
| U9 | 原 `avatarContentUrl / fetchAvatarObjectUrl` | GET `/user/me/avatar/content` | — | 图片 blob | — | 需改造 | Go 契约无内容端点：前端已改为 avatarUrl 直显，不再单独请求该地址 |
| U10 | `charging.js:6 requestFlow` | POST `/user/flows` | `{stationId, chargerType, preferredChargerId}` | flow（含报价快照） | POST `/orders` | 需改造 | 模型差异：Go 按具体 `chargerId` 创建订单，前端只有站点+类型+偏好；需前端先选桩，或 B 线补"站点级下单"契约（B-01 决策） |
| U11 | `charging.js:11 fetchActiveFlow` | GET `/user/flows/active` | — | 活动流程 | — | 后端缺失 | Go 无活动订单端点；可用 `GET /orders?status=<活跃集>` 近似，前端恢复逻辑需改 |
| U12 | `charging.js:16 fetchFlow` | GET `/user/flows/{flowNo}` | — | 流程详情（含待确认报价） | GET `/orders/{orderNo}` | 需改造 | 术语/字段对照；Go 无"待确认报价快照"概念 |
| U13 | `charging.js:21 confirmQuote` | POST `/user/flows/{flowNo}/quote-confirmations` | `{quoteNo, flowVersion}` | 预约结果 | — | 后端缺失 | Go 创建订单即完成选桩与价格快照，无报价确认步骤；前端流程需按订单模型重构 |
| U14 | `charging.js:26 cancelFlow` | POST `/user/flows/{flowNo}/cancellations` | `{reasonCode, flowVersion}` | — | POST `/orders/{orderNo}/cancel` | 需改造 | 路径；Go 不收 `reasonCode/flowVersion`；Go 仅允许未开始订单取消 |
| U15 | `charging.js:31 startFlow` | POST `/user/flows/{flowNo}/start` | `{flowVersion, targetAmountCent, balanceFloorCent}` | 202 命令受理 | POST `/orders/{orderNo}/start` | 需改造 | Go 无目标金额/余额下限（按量模型）；202 异步语义一致（以订单查询确认终态） |
| U16 | `charging.js:40 fetchProgress` | GET `/user/flows/{flowNo}/progress` | — | 秒级进度（电量/金额） | — | 后端缺失 | Go 无进度端点；验收可用订单详情近似，实时进度需 B-01 契约决策 |
| U17 | `charging.js:45 settleFlow` | POST `/user/flows/{flowNo}/settlements` | `{flowVersion, reasonCode}` | `SettlementReceipt` | —（对应 `POST /orders/{orderNo}/stop` + 回执驱动结算） | 需改造 | Go 的"结束并结算"是异步设备闭环：前端改为 POST stop + 轮询订单至 COMPLETED 后取小票 |
| U18 | `charging.js:50 fetchWallet` | GET `/user/wallet` | — | `balanceCent`、`debtCent` | GET `/wallet` | 需改造 | 仅路径；Go 无欠费模型（`debtCent` 恒缺） |
| U19 | `charging.js:55 rechargeWallet` | POST `/user/wallet/recharges` | `{amountCent}`（幂等键） | `balanceCent` | POST `/wallet/top-up` | 需改造 | 仅路径；幂等键长度 16..128 Go 强制（前端已带） |
| U20 | `charging.js:60 fetchWalletTransactions` | GET `/user/wallet/transactions` | `{type, fromAt, toAt, page, pageSize}` | `{items, meta}` | GET `/wallet/transactions` | 需改造 | 仅路径；`fromAt/toAt` 不支持（C8） |
| U21 | `order.js:6 fetchOrders` | GET `/user/orders` | `{status, fromAt, toAt, page, pageSize, sort}` | `{items, meta}` | GET `/orders` | 需改造 | 路径；Go 支持 `status`（字符串枚举）+ 分页；`fromAt/toAt/sort` 不支持 |
| U22 | `order.js:11 fetchOrderReceipt` | GET `/user/orders/{orderNo}` | — | 小票（价格快照/时长/电量/应付/实付/欠费） | GET `/orders/{orderNo}` | 需改造 | 仅路径；字段对照 B-01（Go 无"欠费"字段） |
| U23 | `order.js:16 fetchOrderReview` | GET `/user/orders/{orderNo}/review` | — | `{review}`（可为 null） | GET `/orders/{orderNo}/review` | 需改造 | 路径一致（去 `/user`）；未评价时 Go 404 → 前端映射 `{review: null}`（C9） |
| U24 | `order.js:21 submitOrderReview` | POST `/user/orders/{orderNo}/review` | `{rating, content}`（幂等键） | review | POST `/orders/{orderNo}/review` | 需改造 | 路径；`rating→stars`、`content→comment`；Go 为内容幂等（同内容重放 201），与前端幂等键重试兼容 |
| U25 | `station.js:9 fetchStations` | GET `/user/stations` | `{latitudeE6, longitudeE6, keyword, chargerType, page, pageSize}` | `{items, meta}` | GET `/stations` | 需改造 | 仅路径；`chargerType→connectorType`；Go 另支持半径/空闲数/价格过滤 |
| U26 | `station.js:22 fetchStation` | GET `/user/stations/{stationId}` | — | 站点详情 | GET `/stations/{stationId}` | 需改造 | 仅路径 |
| U27 | `station.js:27 fetchChargers` | GET `/user/stations/{stationId}/chargers` | `{chargerType, status, page, pageSize}` | 设备列表 | GET `/chargers`（`stationId` 为查询参数） | 需改造 | 子路径 → 查询参数；`chargerType` 参数名待对照 |
| U28 | `station.js:32 fetchQuote` | GET `/user/stations/{stationId}/quote` | `{chargerType}` | 预估价 | — | 后端缺失 | Go 无报价端点；展示价可用站点详情价格字段近似（B-01 决策） |
| U29 | `station.js:40 fetchRoute` | GET `/user/stations/{stationId}/route` | `{latitudeE6, longitudeE6, keyword, mode, coordinateType}` | 路线 | — | 后端缺失 | 导航属 A-07 |
| U30 | `station.js:52 fetchStationReviews` | GET `/user/stations/{stationId}/reviews` | — | `{items, meta}`（作者已脱敏） | GET `/stations/{stationId}/reviews` | 需改造 | 仅路径 |
| U31 | `agent.js:16 chatWithAgent` | POST `/user/agent/chat` | `{message, location?, coordinateType?, chargerType?}` | 会话结果 | — | 后端缺失 | **BLOCKED**：等待 A-07 或独立 Agent 模块；A 线按任务文档 §A-05 只做请求层抽象与降级 UI，不得伪装已接通 |

## 3. 管理端清单（apps/admin/src/api/）

| # | 调用位置 | 方法与路径 | 请求体 | 前端消费的响应 | Go OpenAPI 对应 | 状态 | 差距说明 |
|---|---|---|---|---|---|---|---|
| A1 | `auth.js:6 login` | POST `/admin/auth/login` | `{username, password, deviceId}` | `accessToken/expiresAt`、`admin.mustChangePassword` | POST `/auth/admin/login` | 已改造 | 路径一致（去 `/admin`）；**请求体字段已由 `username` 改为 `account`**（Go `LoginRequest` 要求 `account`，发 `username` 会被判为长度越界返回 400）；`deviceId` 被 `api/auth.js` 丢弃、不入请求体；`mustChangePassword` 后端缺失（首登改密流程） |
| A2 | `auth.js:11 reauth` | POST `/admin/auth/reauth` | `{password}` | 15 分钟免重验窗口 | — | 后端缺失 | Go 无重验证机制（错误码 23 未实现）；B-01 决策补契约或 A 线降级（C6） |
| A3 | `auth.js:16 logout` | POST `/admin/auth/logout` | — | — | POST `/auth/logout` | 需改造 | Go 为统一注销端点 |
| A4 | `auth.js:21 changeOwnPassword` | PUT `/admin/me/password` | `{currentPassword, newPassword}` | 清除 `mustChangePassword` | — | 后端缺失 | Go 无管理端改密端点 |
| A5 | `account.js:6 fetchAccounts` | GET `/admin/accounts` | `page/pageSize` | 管理员账号列表（OWNER） | — | 后端缺失 | Go 无管理员账号管理域 |
| A6 | `account.js:11 createAccount` | POST `/admin/accounts` | `{username, password, reason}`（幂等键） | — | — | 后端缺失 | 同上 |
| A7 | `account.js:16 setAccountStatus` | PUT `/admin/accounts/{adminId}/status` | `{status, reason, version}`（幂等键） | — | — | 后端缺失 | 同上 |
| A8 | `charger.js:6 fetchChargers` | GET `/admin/chargers` | `{stationId, status, chargerType, keyword, page, pageSize}` | `{items, meta}` | GET `/admin/chargers` | 需改造 | 路径一致；Go 支持 `keyword/stationId/status`+分页，`chargerType` 参数不支持 |
| A9 | `charger.js:11 createChargersBatch` | POST `/admin/chargers/batch` | `{stationId, chargers[]}`（幂等键） | — | — | 后端缺失 | Go 无建设备端点 |
| A10 | `charger.js:16 setChargerStatus` | PUT `/admin/chargers/{chargerId}/status` | `{targetStatus, reason, version}`（幂等键） | 就地更新该行 | PUT `/admin/chargers/{chargerId}/status` | 需改造 | B 线已实现：body 为 `{status: IDLE\|DISABLED, reason}`（仅两个目标状态；OCCUPIED/RESTARTING 409），无版本乐观锁；前端已接线并限制可选目标 |
| A11 | `charger.js:25 createRestartCommand` | POST `/admin/chargers/{chargerId}/restart-commands` | `{confirm: true, reason}`（幂等键） | 202 `{commandNo, status}` | POST `/admin/chargers/{chargerId}/restart` | 需改造 | 路径；Go 契约未定义 `confirm` 字段（二次确认由前端承担）；响应语义一致 |
| A12 | `charger.js:34 fetchDeviceCommand` | GET `/admin/device-commands/{commandNo}` | — | `PENDING/RUNNING/SUCCEEDED/FAILED` | GET `/admin/device-commands/{commandId}` | 需改造 | B 线已实现：标识为 **commandId**；回执模型为 `{result: COMPLETED\|FAILED, applied}`（回执未到 404），前端已恢复轮询并映射到既有命令状态面板 |
| A13 | `flow.js:6 fetchFlows` | GET `/admin/flows` | `{status, stationId, chargerId, userId, page, pageSize}` | `{items, meta}` | GET `/admin/orders` | 需改造 | 术语+参数：Go 支持 `orderNo/status`+分页，无 `stationId/chargerId/userId` 过滤 |
| A14 | `flow.js:11 forceReleaseFlow` | POST `/admin/flows/{flowNo}/force-releases` | `{confirm, reason, nextChargerStatus, flowVersion}`（幂等键） | — | POST `/admin/chargers/{chargerId}/release` | 需改造 | 按设备而非流程号定位；Go 幂等键必填（前端已带） |
| A15 | `ops.js:6 fetchAuditLogs` | GET `/admin/audit-logs` | `{actorId, action, targetType, targetId, fromAt, toAt, page, pageSize}` | `{items, meta}` | GET `/admin/audit` | 需改造 | 路径；`targetType/targetId→resourceType/resourceId`；时间范围不支持（C8） |
| A16 | `ops.js:11 fetchBackups` | GET `/admin/backups` | — | 备份记录 | — | 后端缺失 | 运维域（B-06 方向），Go 无备份端点 |
| A17 | `ops.js:16 createBackup` | POST `/admin/backups` | `{}`（幂等键） | 202 `backupNo` | — | 后端缺失 | 同上 |
| A18 | `ops.js:21 verifyBackup` | POST `/admin/backups/{backupNo}/verifications` | `{}`（幂等键） | — | — | 后端缺失 | 同上 |
| A19 | `station.js:6 fetchStations` | GET `/admin/stations` | `{status, adcode, keyword, page, pageSize}` | `{items, meta}` | GET `/admin/stations` | 需改造 | 路径一致；`adcode` 不支持，其余参数待 B-01 对照 |
| A20 | `station.js:11 createStation` | POST `/admin/stations` | 站点 + `initialCharger`（幂等键） | — | POST `/admin/stations` | 已匹配 | 路径与方法一致；body（含 `initialCharger` 结构）待 B-01 逐字段对照 |
| A21 | `station.js:16 updateStation` | PUT `/admin/stations/{stationId}` | `{name?/address?/adcode?/latitudeE6?/longitudeE6?/businessHours?/version}`（幂等键） | — | — | 后端缺失 | Go 无修改站点端点 |
| A22 | `station.js:23 setStationEnabled` | POST `/admin/stations/{stationId}/enable\|disable` | `{reason, version}`（幂等键） | — | PUT `/admin/stations/{stationId}/status` | 需改造 | B 线已实现：body 为 `{status: OPEN\|CLOSED\|DISABLED, reason}`，接受 OPEN↔CLOSED 等四种迁移；前端已接线（启停布尔映射 OPEN/DISABLED） |
| A23 | `station.js:34 fetchTariffs` | GET `/admin/tariffs` | `{adcode, effectiveAt, page, pageSize}` | 价格版本列表 | — | 后端缺失 | 模型差异：Go 仅有设备级费率 `GET /admin/chargers/{chargerId}/tariff`，无行政区基础价格版本 |
| A24 | `station.js:39 createTariff` | POST `/admin/tariffs` | 价格版本（幂等键） | — | — | 后端缺失 | 同上 |
| A25 | `station.js:44 createPriceAdjustment` | POST `/admin/price-adjustments` | `{adjustmentBp…}`（幂等键） | — | — | 后端缺失 | Go 无服务费调整域 |
| A26 | `stats.js:39 fetchRevenueStats` | GET `/admin/stats/revenue` | `{fromAt, toAt, stationId, bucket}` | `items[]/total*`（DTO 严格校验） | — | 后端缺失 | 统计属 A-07 |
| A27 | `stats.js:68 fetchChargerStatusStats` | GET `/admin/stats/charger-status` | `{stationId?}` | 八个统计字段 | — | 后端缺失 | 同上 |
| A28 | `user.js:9 fetchUsers` | GET `/admin/users` | `{status, phoneExact, phoneLast4, page, pageSize, sort}` | `{items, meta}` | GET `/admin/users` | 需改造 | 路径一致；Go 支持 `keyword/status`+分页；`phoneExact/phoneLast4/sort` 不支持（隐私口径一致：无模糊扫描） |
| A29 | `user.js:14 fetchUser` | GET `/admin/users/{userId}` | — | 脱敏手机号、钱包汇总、会话数、活动流程摘要 | GET `/admin/users/{userId}` | 需改造 | 路径一致；Go 返回 `{id, phone, displayName, avatarUrl, status, balanceCent, createdAt, deletedAt}`，无会话数/活动流程摘要 |
| A30 | `user.js:19 setUserStatus` | PUT `/admin/users/{userId}/status` | `{status(0|1), reason, version}`（幂等键） | — | POST `/admin/users/{userId}/freeze`、`/unfreeze` | 需改造 | 方法/路径/body 全不同；Go 无 `version` 乐观锁 |
| A31 | `user.js:28 fetchUserOrders` | GET `/admin/users/{userId}/orders` | `{status(60/70/90), page, pageSize}` | `{items, meta}`（写审计） | GET `/admin/orders?userId=` | 需改造 | B 线已实现 userId 过滤：前端已接线（状态数字码 → 字符串枚举，行形状归一化） |
| A32 | `ml.js:17 fetchPredictions` | GET `/admin/predictions` | `{stationId, horizonHour, fromAt}` | 预测结果 | — | 后端缺失 | ML 属 A-07 |
| A33 | `ml.js:23 startMlTask` | POST `/admin/ml-tasks` | `{taskType, horizonHours?}`（幂等键） | `taskNo` | — | 后端缺失 | 同上 |
| A34 | `ml.js:28 fetchMlTask` | GET `/admin/ml-tasks/{taskNo}` | — | 任务状态 | — | 后端缺失 | 同上 |

## 4. Agent（agent/）

- 浏览器侧唯一入口是用户端 U31（`POST /user/agent/chat`）；`agent/` 目录本身是 C++ 服务
  （`CMakeLists.txt` + `agent_service.cpp`），属于 PR #42 的 C++ 服务端改造范围，不是浏览器代码。
- 按任务文档 §A-05：Go 后端未提供 Agent 端点前，A 线只交付请求层抽象、工具调用模型、
  loading/超时/错误/降级 UI 与"未实现"提示，**不得标记为已接通**。

## 5. 第三方与浏览器侧接口（非平台 API，按"不允许未登记的网络请求"一并登记）

| 调用位置 | 目标 | 说明 |
|---|---|---|
| `apps/user/src/services/tencentMap.js:9,72` | `https://map.qq.com/api/gljs?v=1&key=<TENCENT_MAP_JS_KEY>` | 腾讯地图 GL JS SDK；Key 经 `import.meta.env.TENCENT_MAP_JS_KEY` 注入（`.env.example:38`），无硬编码生产 Key；属外部服务，非 C++/SQLite 依赖 |
| `apps/user/src/services/geolocation.js` | 浏览器 `navigator.geolocation` | 设备能力，非 HTTP 请求 |
| `apps/user/src/api/auth.js:61` | 平台头像内容接口 | 已列入 U9 |

## 6. Go 已有但前端未接线的契约（联调验收场景需要，A 线需补调用）

| Go 端点 | 验收场景依赖 | 说明 |
|---|---|---|
| DELETE `/me` | 账号注销（资料域） | 前端未提供注销入口 |
| POST `/admin/orders/{orderNo}/refund` | 退款与账务一致性验证 | 前端管理端无退款入口 |

> 本轮已接线并从本清单移除：用户申诉、资料读写（/me/profile）、管理端申诉队列与审核、按用户查询订单。

## 7. 汇总

| 范围 | 登记数 | 已匹配 | 需改造 | 后端缺失 | BLOCKED |
|---|---|---|---|---|---|
| 用户端 | 31 | 0 | 25 | 5 | 1（U31） |
| 管理端 | 34 | 1（A20） | 16 | 17 | 0 |
| Agent | 1 | — | — | — | 1 |
| 合计 | 66 | 1 | 41 | 22 | 2 |

> 二轮更新（B 线第二批交付后）：U4 注册、U8/U9 头像 URL、A10 桩状态、A12 命令查询（commandId）、
> A22 站点状态、A31 按用户订单由"后端缺失"改为"需改造"并已完成前端接线；A9/A21/A16-A18 等
> 其余后端缺失项状态不变。

> 用户端 U17（结算）按"需改造"计入（语义由 `POST /orders/{orderNo}/stop` + 回执驱动结算承接）。

B 线需要在矩阵中优先裁决的契约决策点：U4（注册）、U10/U13（下单模型）、U11/U16（活动流程与进度）、
U17（结算语义）、U28（报价）、A2（重验证）、A9/A10（设备创建与状态）、A12（命令查询）、
A21–A25（站点修改/启停/价格模型）、A26/A27（统计）、A32–A34（ML）、A16–A18（备份）、
A1/A4（`deviceId`、`mustChangePassword`）、C6（错误码 23）。

## 8. 对 A-02 的落地评估（下一步）

统一请求层**已存在且质量达标**（Bearer、X-Request-ID、Idempotency-Key、信封解包、401/403
会话失效、超时与取消均已实现，且两个 app 各一份、结构一致），A-02 的实际工作是**方言切换**
而非新建层：

1. 信封：`requestId` 回退链加入 `traceId`（两份 `http.js` 各一行级改动，C1）。
2. 路径与字段映射集中到一个常量/适配模块（每 app 一个 `contract.js`），API 模块逐个切换，
   避免散落字符串（U1–U30、A1–A31 中"需改造"项的机械部分）。
3. 状态枚举双向映射表 + 订单轮询改造：`settleFlow` → `POST stop` + 轮询订单终态（C7、U17）。
4. 评价字段 `rating/content → stars/comment`、未评价 404 → `{review: null}`（U23/U24、C9）。
5. 资料聚合：`/me` + `/me/profile` + `/wallet` 三接口替代 `/user/me` 合并视图（U6）。
6. REAUTH 降级开关：在 B-01 裁决前隐藏依赖重验证的管理端操作并提示"暂未开放"（A2、C6）。
7. Agent 降级：`/user/agent/chat` 返回 404/501 时展示"AI 助手暂未开放"（U31）。
8. 每完成一个模块按任务文档 §A-03/§A-04 提交截图、Network 证据、成功/失败样例与自动化测试。

> 二轮同步：上述 1-7 已落地；B 线第二批交付的六项能力（注册、头像 URL、按用户订单、
> 站点状态、充电桩状态、命令查询 commandId）已全部接线并通过两端测试。
