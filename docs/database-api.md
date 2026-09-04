# NCS 数据服务接口文档（HTTPS REST / WebSocket）

| 项目 | 内容 |
| --- | --- |
| 文档版本 | V1.0 |
| 接口版本 | `/api/v1` |
| 通信 | HTTPS REST + JSON；实时事件使用鉴权 WebSocket |
| 数据实现 | Crow Service → SQLite，客户端不得直接访问数据库 |
| 适用模块 | Qt 用户端、Qt 管理端、Web 大屏、ML 子进程 |
| 关联需求 | `UC-U`、`UC-A`、`UC-D`、`UC-W`、`UC-M`、`BR-01`～`BR-12` |
| 兼容策略 | V1 保持向后兼容；破坏性变更使用新版本路径 |

本文是其他模块连接 NCS 数据服务的唯一接口契约。数据库表、SQL、文件路径和 SQLite 错误均属于服务端内部实现，不得出现在客户端代码或公开响应中。

## 1. 通用约定

### 1.1 服务地址与路由边界

开发环境默认地址：

```text
https://127.0.0.1:8443/api/v1
```

| 路由前缀 | 调用方 | 权限 |
| --- | --- | --- |
| `/api/v1/user/*` | Qt 用户端 | 当前车主本人数据 |
| `/api/v1/admin/*` | Qt 管理端 | 运营管理、审计、统计和运维 |
| `/api/v1/dashboard/*` | Web 大屏 | 运营管理员或决策查看者只读 |
| `/api/v1/internal/ml/*` | 本机 ML 子进程 | 一次性任务令牌限定的特征读取和预测回写 |
| `/api/v1/system/*` | 运维检查 | 本机访问；详细信息需要管理员权限 |

监听范围、TLS 和部署边界遵循 SRS 的 `NFR-D-*`；接口层不得暴露数据库文件。

### 1.2 请求头

```http
Accept: application/json
Content-Type: application/json; charset=utf-8
Authorization: Bearer <access_token>
X-Request-ID: <uuid>
Idempotency-Key: <uuid>
```

- `Authorization`：除获取验证码、注册和登录外均必填。用户、管理员、决策查看者和 ML 任务令牌互不通用。
- `X-Request-ID`：建议调用方生成 UUID；缺失时服务端生成。响应始终返回同一值。
- `Idempotency-Key`：产生业务写入的 `POST` 请求必填；查询和纯登录请求除外。
- `Content-Type`：上传头像时为 `multipart/form-data`。
- 不允许客户端传用户 ID 冒充当前用户；用户 ID 从 Bearer Token 会话取得。

### 1.3 统一响应格式

成功：

```json
{
  "success": true,
  "code": 0,
  "message": "",
  "userMessage": "",
  "requestId": "2cb640c6-6995-4be5-9161-f0e2c1201a53",
  "data": {}
}
```

失败：

```json
{
  "success": false,
  "code": 16,
  "message": "quote expired",
  "userMessage": "报价已过期，请重新排队或预约",
  "requestId": "2cb640c6-6995-4be5-9161-f0e2c1201a53",
  "data": null
}
```

- `success` 与 `code` 必须一致，成功恒为 `code=0`。
- `message` 只允许返回无敏感信息的短诊断，不包含 SQL、路径、堆栈、令牌、手机号或验证码。
- 失败时 `userMessage` 必填，可由客户端直接展示。
- 无业务数据的成功响应返回空对象 `{}`，不返回 `null`。
- HTTP 状态码描述传输结果，`code` 描述稳定业务错误；调用方必须优先按 `code` 处理业务分支。

### 1.4 字段约定

公开 JSON 字段使用 `lowerCamelCase`；物理单位、字段后缀和时间格式统一引用[数据库设计 §2](database-design.md#2-类型与命名)。接口不得返回含义不明的 `amount`、`energy`、`power`、`price` 或本地时间字符串。

### 1.5 分页、过滤与排序

列表请求统一参数：

- `page`：从 1 开始，默认 1。
- `pageSize`：默认 20，最大 100。
- `sort`：仅允许各接口列出的白名单值，前缀 `-` 表示降序。
- 未提供过滤参数表示不过滤；空字符串按未提供处理。

统一分页响应：

```json
{
  "items": [],
  "total": 0,
  "page": 1,
  "pageSize": 20
}
```

客户端不得传 SQL、列名、表名、任意 `ORDER BY` 或模糊表达式。手机号管理查询只支持完整手机号精确匹配或后四位匹配，不提供任意 `%keyword%` 扫描。

### 1.6 枚举

状态值引用 SRS 的 `BR-12` 及相应用例，数据库约束引用[数据库设计](database-design.md)。活动流程的未完成/待恢复状态固定为 `10、20、30、40、50、80`，终态为 `60、70、90`；`/flows/active`、活动唯一性检查和注销拦截必须使用同一集合。电桩类型使用 `0/AC_SLOW`、`1/DC_FAST`。接口同时返回整数 `status` 与显示用 `statusText`；代码分支不得依赖翻译文本。

### 1.7 鉴权与会话

- Access Token 使用至少 256 bit 安全随机数，只在登录成功时返回一次；服务端只保存摘要。
- 会话数量、期限、撤销和重新验证规则直接遵循 SRS 的 `NFR-S-04`、`UC-U-01`、`UC-A-01` 与 `UC-W-04`。
- Token 不允许放在 URL、查询参数、日志或 WebSocket 地址中。

### 1.8 幂等与并发

- 预约、确认报价、取消、开始充电、结算、充值、状态变更、重启和批量创建必须携带 `Idempotency-Key`。
- 同一作用域、同一幂等键、相同请求体重复提交时，返回首次提交的相同业务结果。
- 同一幂等键对应不同请求体时返回 `IDEMPOTENCY_CONFLICT`。
- 充值和结算的业务唯一性永久保留；通用幂等响应至少保留 7 天。
- 可编辑资源返回 `version`。更新接口必须提交当前 `version`；版本落后返回 `VERSION_CONFLICT`。
- 客户端收到超时或连接断开时，应使用原幂等键重试，不得生成新键。

### 1.9 传输限制与超时

| 类型 | 限制 |
| --- | --- |
| HTTP 请求体 | JSON 最大 1 MiB；预测批量接口最大 8 MiB |
| 头像 | 最大 5 MiB，解码后最大 4096×4096 |
| 普通请求超时 | 10 秒 |
| 统计请求超时 | 30 秒 |

业务限流、吞吐和 ML 超时引用 SRS 对应条款。超过限流返回 HTTP 429、`RATE_LIMITED`，并在 `data.retryAfterSec` 返回建议等待秒数。

### 1.10 ErrorCode 与 HTTP 映射

| code | 名称 | HTTP | 典型含义 |
| ---: | --- | ---: | --- |
| 0 | `OK` | 200/201 | 成功 |
| 1 | `INVALID_ARGUMENT` | 400 | JSON、格式或单位错误 |
| 2 | `VALIDATION_FAILED` | 422 | 业务字段校验失败 |
| 3 | `DATABASE_ERROR` | 503 | 数据库暂不可用 |
| 4 | `NOT_FOUND` | 404 | 资源不存在 |
| 5 | `ALREADY_EXISTS` | 409 | 唯一资源已存在 |
| 6 | `USER_FROZEN` | 403 | 用户被冻结 |
| 7 | `INSUFFICIENT_BALANCE` | 409 | 不满足最低起充金额 |
| 8 | `CHARGER_UNAVAILABLE` | 409 | 设备不可分配 |
| 9 | `ACTIVE_FLOW_EXISTS` | 409 | 用户已有活动流程 |
| 10 | `ALLOCATION_CONFLICT` | 409 | 并发分配冲突 |
| 11 | `TRANSACTION_FAILED` | 503 | 事务安全回滚 |
| 12 | `EXTERNAL_SERVICE_UNAVAILABLE` | 503 | 地图或 ML 能力失败 |
| 13 | `INTERNAL_ERROR` | 500 | 未预期内部错误 |
| 14 | `IDEMPOTENCY_CONFLICT` | 409 | 幂等键复用到不同请求 |
| 15 | `INVALID_STATE_TRANSITION` | 409 | 非法状态迁移 |
| 16 | `QUOTE_EXPIRED` | 409 | 报价过期 |
| 17 | `RESERVATION_EXPIRED` | 409 | 预约过期 |
| 18 | `DEBT_OUTSTANDING` | 409 | 欠费阻止新流程 |
| 19 | `RATE_LIMITED` | 429 | 请求过于频繁 |
| 20 | `CODE_INVALID` | 422 | 验证码错误或次数超限 |
| 21 | `CODE_EXPIRED` | 422 | 验证码过期 |
| 22 | `VERSION_CONFLICT` | 409 | 乐观锁版本落后 |
| 23 | `REAUTH_REQUIRED` | 401 | 敏感操作需要重新验证 |
| 401 | `UNAUTHORIZED` | 401 | 未登录或会话失效 |
| 403 | `FORBIDDEN` | 403 | 权限不足 |

错误码只增不改。新增错误码必须先更新本文和共享枚举，再实现 Controller、客户端映射和测试。

## 2. 用户认证与资料接口（`/api/v1/user`）

### 2.1 POST `/auth/sms/code` — 获取模拟验证码

无需登录。

请求：

```json
{
  "phone": "13800138000",
  "purpose": "LOGIN"
}
```

`purpose` 可取 `LOGIN`、`REGISTER`、`RESET_PASSWORD`。响应内容与手机号是否已注册无关，统一按冷却、每日上限和容量规则返回，不得暴露手机号是否存在（与 2.3 的防枚举要求一致）；`RESET_PASSWORD` 验证码只会被绑定该手机号的账号验证流程消费。

响应 `data`：

```json
{
  "expiresAt": 1788235800,
  "retryAfterSec": 60,
  "maxAttempts": 5,
  "developmentCode": "123456"
}
```

`developmentCode` 仅在本机开发配置、回环来源和模拟短信模式下出现；生产/验收安全配置必须省略。该字段必须从访问日志、错误日志和追踪系统中过滤。

### 2.2 POST `/auth/register` — 用户名密码注册

请求：

```json
{
  "username": "driver_001",
  "phone": "13800138000",
  "password": "example-password",
  "smsCode": "123456",
  "deviceId": "desktop-a1"
}
```

用户名 3～32 字符，只允许字母、数字和下划线，且不能为纯数字（避免与手机号登录名混淆）；密码 10～128 字符。用户名和手机号分别唯一。

响应 `data`：

```json
{
  "accessToken": "returned-once",
  "expiresAt": 1790827200,
  "sessionId": 41,
  "user": {
    "id": 12,
    "username": "driver_001",
    "phoneMasked": "138****8000",
    "nickname": "用户8000",
    "avatarUrl": null,
    "status": 1,
    "statusText": "正常",
    "registeredAt": 1788235200
  }
}
```

事务内创建用户、密码摘要、钱包账户和会话。

### 2.3 POST `/auth/login/password` — 密码登录

请求：

```json
{
  "loginName": "driver_001",
  "password": "example-password",
  "deviceId": "desktop-a1"
}
```

`loginName` 可以是用户名或完整手机号。账号不存在和密码错误统一返回 `UNAUTHORIZED`，不得暴露账号是否存在。响应结构同注册。

### 2.4 POST `/auth/login/sms` — 验证码登录/自动注册

请求：

```json
{
  "phone": "13800138000",
  "smsCode": "123456",
  "deviceId": "desktop-a1"
}
```

手机号不存在时自动创建用户、钱包和系统用户名；默认昵称为“用户+手机号后四位”。用户可随后通过 `/me/credential` 设置自定义用户名和密码。

### 2.5 POST `/auth/logout` — 退出当前会话

需要用户 Token，无请求体。重复退出返回成功。

### 2.6 GET `/sessions` — 当前用户有效会话

响应 `data`：

```json
{
  "items": [
    {
      "sessionId": 41,
      "deviceId": "desktop-a1",
      "createdAt": 1788235200,
      "lastSeenAt": 1788235500,
      "expiresAt": 1790827200,
      "current": true
    }
  ]
}
```

### 2.7 DELETE `/sessions/{sessionId}` — 撤销指定会话

撤销后目标 Token 立即失效。撤销不存在或已撤销会话按幂等成功处理。

### 2.8 GET `/me` — 获取个人资料

响应包含注册返回的 `user`，并附加：

```json
{
  "balanceCent": 10000,
  "debtCent": 0,
  "hasActiveFlow": false,
  "version": 3
}
```

### 2.9 PUT `/me` — 修改昵称

请求：

```json
{
  "nickname": "新昵称",
  "version": 3
}
```

昵称 1～20 字符且非纯空白。成功返回更新后的完整资料和新 `version`。

### 2.10 POST `/me/avatar` — 上传头像

`multipart/form-data` 字段名 `file`，允许 PNG/JPEG/BMP，最大 5 MiB。服务端必须校验真实格式和尺寸、重新编码并剥离元数据后保存；响应：

```json
{
  "avatarUrl": "/api/v1/user/me/avatar/content",
  "version": 4
}
```

### 2.11 GET `/me/avatar/content` — 读取当前头像

需要当前用户 Token，只返回当前用户自己的头像内容。成功响应使用服务端保存的真实 `Content-Type`，支持 `ETag` 和条件请求；未设置头像时返回 `NOT_FOUND`。响应不得暴露服务端文件路径，注销后的已删除头像不得继续访问。

### 2.12 PUT `/me/credential` — 设置或修改用户名密码

请求：

```json
{
  "username": "driver_001",
  "currentPassword": null,
  "newPassword": "new-example-password",
  "smsCode": "123456"
}
```

已有密码时必须提供 `currentPassword`；无密码或忘记密码时使用已绑定手机号验证码。成功后撤销除当前会话外的其他会话。

### 2.13 DELETE `/me` — 申请注销

请求：

```json
{
  "confirm": true,
  "password": "example-password",
  "smsCode": null
}
```

存在活动流程时返回 `ACTIVE_FLOW_EXISTS`。成功后撤销全部会话、匿名化用户名和手机号并安排头像删除；保留期限遵循 SRS `UC-U-05`。

## 3. 钱包与订单接口（`/api/v1/user`）

### 3.1 GET `/wallet` — 钱包概览

响应：

```json
{
  "balanceCent": 10000,
  "debtCent": 0,
  "availableCent": 10000,
  "version": 7,
  "updatedAt": 1788235200
}
```

### 3.2 POST `/wallet/recharges` — 虚拟充值

需要 `Idempotency-Key`。

请求：

```json
{
  "amountCent": 10000
}
```

范围为 1～1,000,000 分。存在欠费时先清偿欠费，剩余金额进入余额。

响应：

```json
{
  "rechargeNo": "RC202609020001",
  "requestedCent": 10000,
  "debtPaidCent": 1500,
  "balanceAddedCent": 8500,
  "balanceAfterCent": 8500,
  "debtAfterCent": 0,
  "completedAt": 1788235200
}
```

### 3.3 GET `/wallet/transactions` — 钱包流水

参数：`type`、`fromAt`、`toAt`、`page`、`pageSize`；排序固定为 `createdAt` 降序。

### 3.4 GET `/orders` — 我的订单

参数：`status`、`fromAt`、`toAt`、`page`、`pageSize`；`sort` 仅允许 `-createdAt`、`createdAt`。

每项至少返回：

```json
{
  "orderNo": "OR202609020001",
  "flowNo": "FL202609020001",
  "stationName": "NCS 中关村充电站",
  "chargerCode": "ZGC-DC-01",
  "status": 60,
  "statusText": "已完成",
  "startedAt": 1788231600,
  "endedAt": 1788235200,
  "energyMwh": 5200000,
  "amountCent": 702
}
```

### 3.5 GET `/orders/{orderNo}` — 订单小票

仅允许订单所属用户访问。返回完整价格快照、时长、电量、应付、实付、欠费和结算时间，字段与 §5.8 `SettlementReceipt` 一致。

## 4. 站点、设备与价格接口（`/api/v1/user`）

### 4.1 GET `/stations` — 附近站点

参数：`latitudeE6`、`longitudeE6`、`keyword`、`chargerType`、`page`、`pageSize`。`keyword` 是待地理编码的当前位置地址，不作为电站名称过滤条件；解析成功后按所得坐标排序全部匹配类型的电站。经纬度缺失且地址解析失败时使用演示默认位置并在响应中返回 `locationFallback=true`。

响应项：

```json
{
  "id": 1,
  "code": "ZGC",
  "name": "NCS 中关村充电站",
  "address": "北京市海淀区中关村大街 27 号",
  "adcode": "110108",
  "latitudeE6": 39977680,
  "longitudeE6": 116316417,
  "electricityPriceCentPerKwh": 85,
  "servicePriceCentPerKwh": 50,
  "totalPriceCentPerKwh": 135,
  "idleCount": 3,
  "operationalCount": 9,
  "totalCount": 10,
  "distanceMeter": 2300
}
```

### 4.2 GET `/stations/{stationId}` — 站点详情

返回站点基本信息、营业时间、基础价格、实时设备统计和支持的充电类型。

### 4.3 GET `/stations/{stationId}/chargers` — 设备列表

参数：`chargerType`、`status`、`page`、`pageSize`。响应项包含 `id`、`code`、`chargerType`、`powerWatt`、`connectorStandard`、`status`、`statusText`、`totalCount`。

### 4.4 GET `/stations/{stationId}/quote` — 查询当前预估价格

参数：`chargerType`。响应：

```json
{
  "electricityPriceCentPerKwh": 85,
  "baseServicePriceCentPerKwh": 50,
  "queueAdjustmentBp": 1000,
  "mlAdjustmentBp": 0,
  "finalServicePriceCentPerKwh": 55,
  "totalPriceCentPerKwh": 140,
  "calculatedAt": 1788235200
}
```

该接口仅供展示，不能作为结算价格承诺；活动流程报价以 §5.3 返回的 `quoteNo` 和快照为准。

## 5. 充电流程接口（`/api/v1/user`）

### 5.1 POST `/flows` — 请求充电或入队

需要 `Idempotency-Key`。

请求：

```json
{
  "stationId": 1,
  "chargerType": 1,
  "preferredChargerId": null
}
```

服务端校验账号、欠费、最低余额和活动流程唯一性。有兼容空闲设备时生成待确认报价；无设备时进入按“站点+类型”的 FIFO 队列。

响应 `FlowDto`：

```json
{
  "flowNo": "FL202609020001",
  "stationId": 1,
  "chargerType": 1,
  "chargerId": null,
  "chargerCode": null,
  "status": 10,
  "statusText": "排队中",
  "queuePosition": 3,
  "quote": null,
  "reservedUntil": null,
  "startedAt": null,
  "version": 1
}
```

### 5.2 GET `/flows/active` — 获取当前活动流程

本接口把 `status IN (10,20,30,40,50,80)` 视为未完成或待恢复流程；同一用户最多返回一个。状态 60、70、90 不返回。

无活动流程时返回：

```json
{
  "hasActiveFlow": false,
  "flow": null
}
```

这是登录、重连和程序重启后恢复页面的唯一入口。

### 5.3 GET `/flows/{flowNo}` — 获取流程详情

待报价时 `quote` 返回：

```json
{
  "quoteNo": "QT202609020001",
  "chargerId": 8,
  "chargerCode": "ZGC-DC-02",
  "electricityPriceCentPerKwh": 85,
  "baseServicePriceCentPerKwh": 50,
  "queueAdjustmentBp": 1000,
  "mlAdjustmentBp": 0,
  "finalServicePriceCentPerKwh": 55,
  "totalPriceCentPerKwh": 140,
  "expiresAt": 1788235500
}
```

### 5.4 POST `/flows/{flowNo}/quote-confirmations` — 确认报价并预约

请求：

```json
{
  "quoteNo": "QT202609020001",
  "flowVersion": 2
}
```

报价有效期 5 分钟。成功后创建订单并进入 15 分钟预约状态：

```json
{
  "flowNo": "FL202609020001",
  "orderNo": "OR202609020001",
  "status": 30,
  "statusText": "已预约",
  "chargerId": 8,
  "chargerCode": "ZGC-DC-02",
  "reservedUntil": 1788236400,
  "version": 3
}
```

### 5.5 POST `/flows/{flowNo}/cancellations` — 取消流程

请求：

```json
{
  "reasonCode": "USER_CANCELLED",
  "flowVersion": 3
}
```

仅排队、待报价和预约状态允许用户取消。重复取消返回同一终态，不重复释放设备或递补。

### 5.6 POST `/flows/{flowNo}/start` — 开始充电

请求：

```json
{
  "flowVersion": 3,
  "targetAmountCent": null,
  "balanceFloorCent": null
}
```

成功响应：

```json
{
  "flowNo": "FL202609020001",
  "orderNo": "OR202609020001",
  "status": 40,
  "statusText": "充电中",
  "startedAt": 1788235200,
  "powerWatt": 60000,
  "timeScale": 60,
  "version": 4
}
```

服务端再次校验账号、欠费、余额、预约归属、预约期限和设备状态。倍率在开始时快照，后续配置变化不影响该流程。

### 5.7 GET `/flows/{flowNo}/progress` — 当前充电进度

响应：

```json
{
  "flowNo": "FL202609020001",
  "orderNo": "OR202609020001",
  "status": 40,
  "statusText": "充电中",
  "durationSec": 600,
  "energyMwh": 10000000,
  "amountCent": 1400,
  "powerWatt": 60000,
  "simulatedSoc": 55,
  "calculatedAt": 1788235300
}
```

服务端根据时间和订单快照计算，不执行每秒数据库写入。客户端可每秒轮询；优先使用 WebSocket `charge.progress` 事件。

### 5.8 POST `/flows/{flowNo}/settlements` — 结束充电并结算

请求：

```json
{
  "flowVersion": 4,
  "reasonCode": "USER_STOPPED"
}
```

成功响应 `SettlementReceipt`：

```json
{
  "flowNo": "FL202609020001",
  "orderNo": "OR202609020001",
  "stationName": "NCS 中关村充电站",
  "chargerCode": "ZGC-DC-02",
  "startedAt": 1788235200,
  "endedAt": 1788238800,
  "durationSec": 3600,
  "energyMwh": 60000000,
  "electricityPriceCentPerKwh": 85,
  "servicePriceCentPerKwh": 55,
  "amountCent": 8400,
  "paidCent": 8000,
  "debtAddedCent": 400,
  "balanceAfterCent": 0,
  "debtAfterCent": 400,
  "settledAt": 1788238800,
  "status": 60,
  "statusText": "已完成"
}
```

订单、钱包、欠费、钱包流水、设备释放、设备累计值、流程状态和通知 outbox 必须在同一事务完成。重复请求返回同一小票。

## 6. 管理员认证与用户管理（`/api/v1/admin`）

### 6.1 POST `/auth/login` — 管理员登录

请求：

```json
{
  "username": "admin",
  "password": "example-password",
  "deviceId": "admin-desktop-a1"
}
```

响应返回一次性 `accessToken`、会话到期时间、管理员资料和角色列表。默认演示账号仅允许开发种子，正式配置首次登录必须改密。

### 6.2 POST `/auth/reauth` — 敏感操作重新验证

请求：`{"password":"example-password"}`。成功返回 `reauthExpiresAt`，有效 15 分钟。

### 6.3 POST `/auth/logout` — 管理员退出

重复退出幂等成功。

### 6.4 GET `/users` — 用户列表

参数：`status`、`phoneExact`、`phoneLast4`、`page`、`pageSize`、`sort`。排序白名单：`registeredAt`、`-registeredAt`、`balanceCent`、`-balanceCent`。

### 6.5 GET `/users/{userId}` — 用户详情

返回脱敏手机号、资料、钱包汇总、会话数量和活动流程摘要，不返回密码、令牌或验证码摘要。

### 6.6 PUT `/users/{userId}/status` — 冻结或解冻

需要 `Idempotency-Key` 和 `OPERATOR` 权限。

请求：

```json
{
  "status": 0,
  "reason": "人工审核冻结",
  "version": 3
}
```

冻结不取消进行中的充电，但立即撤销登录会话并阻止新流程；进行中流程继续由服务端计费并可正常结算。

### 6.7 GET `/users/{userId}/orders` — 用户订单历史

参数与用户端订单列表相同。管理员访问必须写审计日志。

## 7. 管理员站点、设备与价格（`/api/v1/admin`）

### 7.1 GET `/stations` — 站点列表

参数：`status`、`adcode`、`keyword`、`page`、`pageSize`。

### 7.2 POST `/stations` — 新增站点

需要 `Idempotency-Key`。请求中的初始设备配置用于完成 SRS `UC-A-06` 的组合创建：

请求：

```json
{
  "code": "ZGC2",
  "name": "NCS 中关村二站",
  "address": "北京市海淀区示例路 1 号",
  "adcode": "110108",
  "latitudeE6": 39977680,
  "longitudeE6": 116316417,
  "businessHours": "00:00-24:00",
  "initialCharger": {
    "count": 4,
    "chargerType": 1,
    "powerWatt": 60000,
    "connectorStandard": "GB/T 20234.3"
  }
}
```

`initialCharger` 必填，`count` 为 1～100；服务端使用站点 `code` 按 `站点编码-DC/AC-两位序号` 生成不重复设备编号。站点和初始设备必须在同一事务中全部创建或全部回滚。`adcode` 必须存在当前生效的区域基础价格版本；本接口不直接创建或修改价格。

### 7.3 PUT `/stations/{stationId}` — 修改站点

请求只允许包含 `name`、`address`、`adcode`、`latitudeE6`、`longitudeE6`、`businessHours` 和当前 `version`；不得包含 `initialCharger`。不允许通过该接口直接修改价格，行政区变化后必须能解析到生效的区域基础价格版本。

### 7.4 POST `/stations/{stationId}/disable` — 停用站点

请求：`{"reason":"维护停用","version":3}`。存在活动流程时返回 `INVALID_STATE_TRANSITION`；站点停用不物理删除历史数据。

### 7.5 POST `/stations/{stationId}/enable` — 启用站点

需要原因和当前版本。

### 7.6 GET `/chargers` — 设备列表

参数：`stationId`、`status`、`chargerType`、`keyword`、`page`、`pageSize`。

### 7.7 POST `/chargers/batch` — 批量创建设备

请求：

```json
{
  "stationId": 1,
  "chargers": [
    {
      "code": "ZGC-DC-11",
      "chargerType": 1,
      "powerWatt": 120000,
      "connectorStandard": "GB/T 20234.3"
    }
  ]
}
```

最多一次创建 100 台，全部成功或全部回滚。

### 7.8 PUT `/chargers/{chargerId}/status` — 设置设备状态

请求：

```json
{
  "targetStatus": 2,
  "reason": "人工巡检发现故障",
  "version": 7
}
```

活动设备不得直接变为空闲、故障或停用，必须使用受控释放或重启接口。

### 7.9 POST `/chargers/{chargerId}/restart-commands` — 远程重启

需要重新验证、`Idempotency-Key` 和必填原因。

请求：

```json
{
  "confirm": true,
  "reason": "远程恢复测试"
}
```

响应 HTTP 202：

```json
{
  "commandNo": "CMD202609020001",
  "status": "PENDING",
  "chargerStatus": 4,
  "createdAt": 1788235200
}
```

`chargerStatus` 为设备实时状态：0 闲置、1 在用、2 故障、3 已停用、4 重启中。创建命令成功后设备立即进入"重启中"，两秒模拟执行期内不可再分配给用户。

充电中设备必须先受控结束并幂等结算，任一步失败均不得直接释放设备。

### 7.10 GET `/device-commands/{commandNo}` — 查询设备命令

返回 `PENDING/RUNNING/SUCCEEDED/FAILED`、开始/完成时间和安全错误信息。

### 7.11 GET `/tariffs` — 基础价格版本

参数：`adcode`、`effectiveAt`、`page`、`pageSize`。

### 7.12 POST `/tariffs` — 创建基础价格版本

请求：

```json
{
  "adcode": "110108",
  "electricityPriceCentPerKwh": 85,
  "servicePriceCentPerKwh": 50,
  "effectiveFrom": 1788235200,
  "effectiveTo": null,
  "reason": "年度基础价格"
}
```

同一行政区有效期不得重叠。已经生成的订单价格快照不受影响。

### 7.13 POST `/price-adjustments` — 批准服务费调整

请求：

```json
{
  "stationId": 1,
  "chargerType": 1,
  "source": "ML_APPROVED",
  "adjustmentBp": 500,
  "effectiveFrom": 1788235200,
  "effectiveTo": 1788256800,
  "reason": "未来 6 小时负荷预测偏高"
}
```

`adjustmentBp` 范围 -2000～2000，步长 500；只调整服务费。最终服务费必须限制在基础服务费的 80%～140%。

## 8. 管理员流程、统计、审计和备份（`/api/v1/admin`）

### 8.1 GET `/flows` — 活动流程列表

参数：`status`、`stationId`、`chargerId`、`userId`、`page`、`pageSize`。

### 8.2 POST `/flows/{flowNo}/force-releases` — 强制释放

仅状态 20 或 30 可用，需要重新验证和 `Idempotency-Key`。

请求：

```json
{
  "confirm": true,
  "reason": "设备计划维护",
  "nextChargerStatus": 2,
  "flowVersion": 3
}
```

充电中状态返回 `INVALID_STATE_TRANSITION`，应使用设备重启命令的受控结算流程。

### 8.3 GET `/stats/revenue` — 营收统计

参数：`fromAt`、`toAt`、`stationId`、`bucket=day|hour`。时间范围最大 90 天；默认最近 30 天。

### 8.4 GET `/stats/charger-status` — 设备状态统计

参数：`stationId`。返回五种状态数量、可运营数和健康度。

### 8.5 GET `/audit-logs` — 审计日志

需要 `OWNER` 权限。参数：`actorId`、`action`、`targetType`、`targetId`、`fromAt`、`toAt`、`page`、`pageSize`。不提供修改和删除接口。

### 8.6 POST `/backups` — 创建一致性备份

需要 `OWNER` 权限、重新验证和 `Idempotency-Key`。响应 HTTP 202，返回 `backupNo` 和任务状态。

### 8.7 GET `/backups` — 备份记录

只返回编号、校验和、大小、状态和时间，不返回可由浏览器读取的真实文件路径。

### 8.8 POST `/backups/{backupNo}/verifications` — 隔离恢复验证

需要 `OWNER` 权限。验证使用独立临时路径，禁止覆盖当前数据库。

## 9. 预测接口（`/api/v1/admin`）

### 9.1 GET `/predictions` — 查询预测

参数：`stationId`、`horizonHour=1|6|24`、`fromAt`。响应包含模型版本、生成时间、目标时间、预测电量、预测空闲数、高峰标志和过期标志。

### 9.2 POST `/ml-tasks` — 启动训练或预测

请求：

```json
{
  "taskType": "PREDICT",
  "horizonHours": [1, 6, 24]
}
```

响应 HTTP 202。相同类型任务正在运行时返回现有任务，不重复启动。

### 9.3 GET `/ml-tasks/{taskNo}` — 查询 ML 任务

返回 `PENDING/RUNNING/SUCCEEDED/FAILED/TIMED_OUT`、模型版本、指标摘要和安全错误信息。

## 10. Web 大屏接口（`/api/v1/dashboard`）

### 10.1 POST `/auth/login` — 大屏登录

不开放注册。运营管理员和决策查看者使用管理员凭据体系登录；请求：

```json
{
  "username": "viewer_001",
  "password": "example-password",
  "deviceId": "dashboard-browser-a1"
}
```

服务端只允许状态正常且具有 `OPERATOR`、`OWNER` 或 `VIEWER` 角色的账号登录。响应返回一次性 `accessToken`、`expiresAt`、`sessionId`、脱敏账号资料和角色列表。Dashboard Token 只允许访问 `/api/v1/dashboard/*` 和 WebSocket，不得调用 `/api/v1/admin/*` 修改接口；账号不存在和密码错误统一返回 `UNAUTHORIZED`。会话最长8小时，连续空闲30分钟后失效。

### 10.2 POST `/auth/logout` — 大屏退出

需要 Dashboard Token。重复退出幂等成功；退出后立即撤销当前会话并停止该会话的 WebSocket 推送。

### 10.3 GET `/summary` — 大屏完整快照

需要运营管理员或决策查看者 Token。响应：

```json
{
  "schemaVersion": 1,
  "dataVersion": 1832,
  "generatedAt": 1788235200,
  "stale": false,
  "totalRevenueCent": 9800000,
  "registeredUserCount": 300,
  "stationCount": 5,
  "chargerStatus": {
    "idle": 30,
    "inUse": 12,
    "fault": 4,
    "restarting": 1,
    "disabled": 1
  },
  "revenue30d": [],
  "stationRanking": [],
  "hourlyHeatmap": [],
  "prediction24h": []
}
```

大屏首次加载、WebSocket 断线重连或检测到版本跳跃时必须重新获取完整快照。服务端同时每 30 秒原子导出同结构 `dashboard.json` 作为离线降级，但该文件不得成为匿名公开的数据源。

## 11. ML 内部接口（`/api/v1/internal/ml`）

内部接口只监听回环地址。Crow 启动 ML 子进程时签发一次性任务令牌，令牌限定 `taskNo`、接口作用域和到期时间；不得复用用户或管理员 Token。

### 11.1 GET `/features/hourly` — 导出小时特征

参数：`taskNo`、`fromAt`、`toAt`、`stationId`、`cursor`、`limit`。`limit` 最大 5000，使用游标分页，不使用大偏移量分页。

响应条目包含 `stationId`、`stationName`、`bucketAt`、`energyMwh`、`orderCount`、`fastOrderCount`、`slowOrderCount`、`operationalChargerCount` 和 `busyDeviceSeconds`；无订单小时也返回零值。响应的 `nextCursor` 为 `null` 时分页结束。

### 11.2 POST `/model-versions` — 登记模型版本

提交 `taskNo`、算法、特征 schema 版本、固定随机种子、训练区间、MAE/RMSE/MAPE/WAPE、朴素基线 MAE/RMSE、排除样本数和产物校验和。服务端以 MAE 与 RMSE 均优于基线判定 `qualified`。模型产物路径由服务端生成，ML 不得提交任意文件路径；产物写入任务专属暂存文件，摘要校验且任务成功后才原子激活。

### 11.3 POST `/predictions/batch` — 批量回写预测

需要 `Idempotency-Key`。单批最多 5000 行：

```json
{
  "taskNo": "ML202609020001",
  "modelVersionNo": "MODEL202609020001",
  "items": [
    {
      "stationId": 1,
      "generatedAt": 1788235200,
      "targetAt": 1788238800,
      "horizonHour": 1,
      "predictedEnergyMwh": 120000000,
      "predictedFreeCount": 3,
      "isPeak": true
    }
  ]
}
```

服务端校验任务、模型版本、时间范围和数值边界后事务化 upsert。相同模型、站点和目标时间唯一。

### 11.4 POST `/tasks/{taskNo}/completion` — 完成任务

请求包含 `status=SUCCEEDED|FAILED`、可选 `modelVersionNo`、指标摘要和安全错误摘要。成功训练必须引用该任务已登记的模型版本；完成后立即撤销任务令牌。失败时保留最近成功模型和预测，并标记过期。

## 12. 系统接口（`/api/v1/system`）

### 12.1 GET `/health/live` — 进程存活

无需登录，仅返回 `{"status":"UP"}`，不检查数据库、不暴露版本或路径。

### 12.2 GET `/health/ready` — 服务就绪

仅回环地址或管理员可访问。检查 schema 版本、数据库可读写、WAL 和迁移状态；只返回布尔检查结果，不返回数据库路径和 SQL 错误。

## 13. WebSocket 实时事件

连接地址：

```text
wss://127.0.0.1:8443/api/v1/events
```

### 13.1 握手与鉴权

Token 通过握手 `Authorization: Bearer <token>` 传递，不放入 URL。允许用户、管理员和大屏令牌；ML 任务令牌不得连接。握手拒绝以普通 HTTP 响应返回，不升级连接：缺失或格式错误的凭证、未知/过期/已撤销的令牌返回 401；ML 令牌或超出连接数上限（默认 100，可配置）返回 403。服务端限制单条入站消息最大 64 KiB（可配置），超限由服务端以 1009 关闭。同一会话（同一令牌）已有连接时，新连接取代旧连接：旧连接以 1001 关闭，不影响新连接接收事件（断线重连无需等待心跳清理）。

### 13.2 事件信封

连接成功后服务端立即发送 `session.ready`：

```json
{
  "type": "session.ready",
  "eventId": "EV2026090200000001",
  "sequence": 1832,
  "occurredAt": 1788235200,
  "data": {}
}
```

- `type`：事件类型，`session.ready` 或下表六类业务事件之一。
- `eventId`：`EV` + UTC `YYYYMMDD` + 8 位日序号，进程内唯一；UTC 零点日序号重置。日序号每天最多 10⁸ 条，超出后取模回绕。
- `sequence`：全局 `uint64`，跨全部连接和事件类型严格递增，进程内从 1 开始；`session.ready` 同样消耗一个序号。进程重启后序号重置，客户端必须按断线处理并重新执行 REST 快照恢复。
- `occurredAt`：业务事件发生的 UTC Unix 秒（outbox 事件为业务写入时刻，不是投递时刻）。
- `data`：见 13.3 各事件白名单。

事件类型：

| 事件 | 接收者 | 用途 |
| --- | --- | --- |
| `flow.updated` | 对应用户、管理员 | 排队位置、报价、预约和状态变化 |
| `charge.progress` | 对应用户 | 每秒充电展示数据；可丢弃旧事件 |
| `charger.statusChanged` | 管理员 | 设备状态变化 |
| `order.settled` | 对应用户、管理员 | 结算完成通知，不携带完整小票 |
| `dashboard.refresh` | 大屏角色 | 通知拉取新版本快照 |
| `session.revoked` | 对应会话 | 立即退出 |

### 13.3 各事件 `data` 白名单

**`flow.updated`**（对应用户与全部管理员）：

```json
{
  "flowNo": "FL202609020001",
  "fromStatus": 10,
  "toStatus": 20,
  "statusText": "待报价",
  "reasonCode": "CREATED",
  "stationId": 1,
  "stationName": "NCS 中关村充电站",
  "chargerCode": "ZGC-DC-01",
  "queuePosition": 3
}
```

前五个字段始终存在；`stationId`、`stationName`、`chargerCode`、`queuePosition` 为尽力补充字段，未知时省略；`queuePosition` 仅当流程处于排队状态时出现。

**`charge.progress`**（对应用户，约每秒一次，可丢弃）：字段与 §5.7 进度 DTO 一致：

```json
{
  "flowNo": "FL202609020001",
  "orderNo": "OR202609020001",
  "status": 40,
  "statusText": "充电中",
  "durationSec": 3720,
  "energyMwh": 12345678,
  "amountCent": 1234,
  "powerWatt": 60000,
  "simulatedSoc": 36,
  "calculatedAt": 1788235200
}
```

进度按充电开始快照与时间倍率计算，推送节奏不改变业务数据语义。

**`charger.statusChanged`**（全部管理员）：

```json
{
  "chargerId": 3,
  "chargerCode": "ZGC-DC-03",
  "stationId": 1,
  "fromStatus": 0,
  "toStatus": 1,
  "reason": "ALLOCATED"
}
```

状态值：0 闲置、1 在用、2 故障、3 停用、4 重启中。

**`order.settled`**（对应用户与全部管理员，不含完整小票）：

```json
{
  "orderNo": "OR202609020001",
  "flowNo": "FL202609020001",
  "amountCent": 1234,
  "energyMwh": 12345678,
  "settledAt": 1788235200,
  "status": 60
}
```

明确不含 `paidCent`、`debtAddedCent`、`balanceAfterCent`、`debtAfterCent`、价格与站点字段。

**`dashboard.refresh`**（大屏）：`data` 为 `{}`。客户端收到后通过 `GET /api/v1/dashboard/summary` 重新获取完整快照，快照的 `dataVersion` 等于本帧的 `sequence`。

**`session.revoked`**（仅该会话）：`data` 为 `{"reason":"revoked"}`，服务端随后立即以 4001 关闭连接。

### 13.4 心跳与关闭码

服务端在连接静默（未收到 pong）30 秒后发送应用层帧 `{"type":"ping"}`，客户端必须回复 `{"type":"pong"}`；60 秒未收到 pong 时服务端以 4000 关闭连接。客户端发送的协议级 ping 由服务端按 RFC 6455 自动回 pong。心跳回复同时刷新会话空闲计时。

| 关闭码 | 含义 |
| --- | --- |
| 1000 | 客户端正常关闭 |
| 1001 | 服务端关闭（含同一会话被新连接取代） |
| 1013 | 服务端积压溢出或连接数达到上限 |
| 4000 | 心跳超时 |
| 4001 | 会话已撤销（紧随 `session.revoked` 之后） |
| 4002 | 会话已过期或失效 |

### 13.5 积压与投递语义

服务端对每个连接限制 60 秒滑动窗口内的投递量：默认 256 帧或 1 MiB（可配置）。窗口溢出时先放弃窗口内已计入的 `charge.progress` 帧腾出容量；仍溢出时丢弃新到的 `charge.progress` 帧，其他帧则导致连接以 1013 关闭。客户端不得把任何推送帧视为持久数据。

投递为至少一次：进程运行期间同一事件不重复投递；进程重启后未投递完成的事件会以新的 `eventId` 和 `sequence`、原始 `occurredAt` 重新投递。投递失败的行按指数退避重试（首次约 5 秒，上限 300 秒），累计十次失败后转为死信不再投递。每个事件包含单调递增 `sequence`；客户端发现序号跳跃、断线或重连时，必须调用相应 REST 快照接口恢复，不得假设增量事件完整。通知不得包含完整手机号、钱包余额、令牌、验证码或完整订单。

## 14. 接口实现边界

- Controller 只负责协议、鉴权、字段解析和响应序列化；业务规则、事务和权限判断位于 Service。
- 所有 JSON 字段采用严格白名单；未知字段默认拒绝，避免调用方误以为字段已生效。
- 全局异常中间件把未处理异常映射为 `INTERNAL_ERROR`，详细堆栈只进入受保护服务端日志。
- 输入、上传、日志、CORS、TLS 和数据库失败处理遵循 SRS 的 `NFR-S-*`、`NFR-R-*`；本节只规定协议层不得泄露内部实现。

## 15. 事务边界

接口对应的原子写入范围统一引用[数据库设计 §6](database-design.md#6-事务边界)。任何事务失败都返回失败结果，不允许通过 API 表达部分成功；批量接口的原子范围必须在接口小节中明确。

## 16. 版本与变更

1. V1 路径、HTTP 方法、字段名、单位、状态值和错误码属于公开契约。
2. 新增可选响应字段属于向后兼容变更；删除字段、重命名、改变单位、改变状态值或收紧合法范围属于破坏性变更。
3. 破坏性变更必须新增 `/api/v2` 或提供完整迁移期，不得直接修改 V1 行为。
4. 新接口或字段先更新本文和共享 DTO，再实现服务端、客户端与契约测试。
5. 每个写接口至少测试正常流、权限、字段边界、非法状态、并发、幂等重试和事务回滚。
6. 每个查询接口至少测试空数据、分页上限、过滤、排序白名单、权限和性能预算。
7. 接口实现完成以自动化契约测试和端到端证据为准，文档存在不代表功能已经实现。
