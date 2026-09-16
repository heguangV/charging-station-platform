# 迁移模块审批单：BE-I-02 充电设备控制与回执闭环（跨线）

本文件按 `docs/migration/module-approval-template.md` 填写。

**状态说明（第 3 版）：范围与契约维持第 2 版不变，本版补入实现与验证记录（见"验证记录"）。** 第 1 版登记方向
被接受，但契约未冻结完整（5 项 P1/P2 已在第 2 版修正，并补入集成人员的冻结决定）。本模块由 BE-I-01 交付过程中
发现的两个契约缺口而来：这两个缺口不否定 BE-I-01 已完成的事件基础设施，但使"后端 P0 已完整闭环"的结论无法成立。
集成人员决定单独立项，并在 H5 业务开发前完成。

**当前状态**：两项交付均已实现并通过真实 PostgreSQL + Redis 验证；集成人员已在 `052270e` 完成回执端点的
OpenAPI 登记。首轮审查的 3 项问题已于第 4 版修复；第二轮审查（`b7f8c11`）指出第 4 版的 TTL 取舍不成立，
**第 5 版改用不设过期的 `charger_command_outcomes`（新增顺序迁移 0007）**并补充跨幂等窗口的回归用例与反向
验证（见"第 4 版遗留项"）。最终复审通过，审批结论为 APPROVED；待合并门更新 `approval-log.md`。

## 基本信息

- 模块编号：BE-I-02
- 模块名称：充电设备控制与回执闭环
- 开发线：**跨线**（A 线 + B 线同时改动）
- 开发分支：`codex/migration/i-02-charger-control-receipts`（集成人员已冻结）
- 基线提交：**BE-I-01 最终批准并合入集成分支之后的提交**
- 目标集成分支：`codex/migration-integration`

### 它解决什么问题

BE-I-01 的闭环验证把订单状态推进到 `STARTING` 就停下了，因为状态机无法再被驱动：

| 缺口 | 现状 | 影响 |
|---|---|---|
| 设备回执没有入口 | `order.Service.ConfirmStart` / `ConfirmStop` 会**在同一事务里**写状态并追加 `CHARGE_STARTED` / `CHARGE_STOPPED` 事件，但没有任何已登记的接口调用它们 | `STARTING → CHARGING → STOPPING → COMPLETED` 无法发生，"完整 P0 订单闭环"不成立 |
| 命令动作集不完整 | `worker.SupportedCommandActions()` 只有 `RESTART` | 平台无法命令充电桩开始/停止充电；用 `RESTART` 冒充是语义错误 |

## 交付范围（固定为两项）

### 1. 设备回执端点

```text
POST /api/v1/internal/charger-events
```

- **鉴权（已冻结）**：HTTPS + `Authorization: Bearer <服务令牌>`。令牌从 `NCS_CHARGER_GATEWAY_TOKEN` 读取，
  **无默认值**（未配置即拒绝启动），比较使用常量时间比较；同时由 Nginx 限制仅在充电桩网关来源网络可达。
  该端点能推进订单并触发计费，因此鉴权方式不允许留到实现时再选。
- **载荷**（至少包含）：

```text
eventId       回执唯一标识，幂等键
eventType     CHARGE_STARTED | CHARGE_STOPPED
orderNo       订单号
chargerId     充电桩标识
occurredAt    设备侧发生时间（RFC3339）
energyWh      已充电量（STOPPED 必填）
meterStartWh  起始电表读数（可选，提供则参与校验）
meterEndWh    结束电表读数（可选，提供则与 energyWh 一致性校验）
traceId       链路追踪
```

- **时间语义（已冻结）**：`occurredAt` 是**业务事实时间**（设备侧实际发生的时刻），不是审计字段：
  - 必须是 UTC（缺失或非 UTC → 400）；
  - `CHARGE_STARTED`：用它写入 `charging_orders.started_at`；
  - `CHARGE_STOPPED`：用它写入 `stopped_at` / `completed_at`，并**作为分时电价（TOU）计算的依据**；
  - 停止时间早于开始时间 → **409**；
  - 超过服务器当前时间 **5 分钟**（配置项，默认 5 分钟）→ **400**；
  - 重复回执（同 `eventId` 同载荷）→ 返回**首次计算结果**，不重算。
  接口变化：`order.ConfirmStartCommand` / `ConfirmStopCommand` 增加事实时间字段，`ConfirmStart` / `ConfirmStop`
  用该时间写库并参与计费；服务器时间只用于租约、审计与"未来偏差"判断。
  这一条是必需的：离线补报或网络延迟的回执若用服务器时间入库，分时电价会算在最错误的窗口上。
- **行为**：校验订单与充电桩的归属关系 → 调用既有 `order.Service.ConfirmStart` / `ConfirmStop` →
  由其事务性推进状态并写 Outbox。**处理器不得自行写订单表或 Outbox**，否则两处逻辑会漂移；
- **幂等（已冻结）**：复用 PostgreSQL `idempotency_records`，以 `eventId` 为 key、**请求体摘要**为 `request_hash`，
  并在 `ConfirmStart`/`ConfirmStop` 的**同一事务内**完成 claim 与 finalize。同 ID 同载荷 → 返回首次结果；
  同 ID 不同载荷 → 409。
  **消费记录（`event_consumptions`）不能保护这个入口**：它去重的是之后从 Stream 消费到的通知事件，
  而 HTTP 回执在进入 Stream 之前就已经调用了领域方法，重复回执会重复执行。第 1 版把这一点写错了，本版修正。
- **拒绝**：订单不存在、订单与充电桩不匹配、订单状态不允许该确认 → 明确的 4xx，且不写任何状态。

登记的 OpenAPI 片段由集成人员完成（第 4.1 节：先登记再实现），A 线随后实现。

### 2. 命令动作集扩展与命令的产生路径

```text
RESTART
START_CHARGING
STOP_CHARGING
```

- **不得用 `RESTART` 代替开始/停止**；
- **谁产生命令事件（已冻结）**：`order.Service.Start`（`StartCharging`）在**原事务内**除既有
  `CHARGE_START_REQUESTED` 之外，再追加一条 `CHARGER_COMMAND_REQUESTED`，`action = START_CHARGING`；
  `Stop`（`StopCharging`）同理追加 `STOP_CHARGING`。生命周期通知事件**保留**：两者语义不同，前者说明
  "订单状态已推进、生命周期发生了什么"，后者是"要给设备下一条命令"。
  只扩展动作枚举不会产生任何命令：今天 `CHARGE_START_REQUESTED` 只被 Worker 当通知消费（BE-I-01 已冻结该语义），
  因此必须由订单事务额外产生命令事件。
- **命令载荷（已冻结）**：

```text
command_id     命令标识（幂等键）
charger_id     充电桩标识
order_no       订单号，START_CHARGING / STOP_CHARGING 必填，RESTART 不需要
action         RESTART | START_CHARGING | STOP_CHARGING
trace_id       链路追踪
```

  `order_no` 是必需的：网关需要一个能确认"这条命令对应哪张订单"的标识，否则无法把回执关联回订单。
  RESTART 是站点级运维动作、与订单无关，因此不带该字段。
- 需要同时更新的位置：事件契约（`api/openapi.yaml` 与事件文档的动作取值）、
  `worker.SupportedCommandActions`、`CommandAction` 常量、`parseChargerCommand` 校验、
  `cmd/worker/adapters.go` 的 Dispatcher（含各动作的失败语义）、`cmd/mock-gateway`（模拟器应答）、
  以及契约测试；
- 每个动作的**成功与失败路径**都要有测试：设备拒绝、超时、未知充电桩、动作与充电桩状态不匹配。

### 3. 状态推进与失败语义（已冻结）

| 事件 | 结果 | 订单 | 充电桩 |
|---|---|---|---|
| 命令被设备接受 | 成功 | **不推进**（命令成功只表示设备接受命令） | 维持 |
| `CHARGE_STARTED` 回执 | 成功 | `STARTING → CHARGING` | `OCCUPIED` |
| `CHARGE_STOPPED` 回执 | 成功 | `STOPPING → COMPLETED` | 释放 |
| `START_CHARGING` 明确失败 | 失败 | `STARTING → FAILED` | `FAULT` |
| `STOP_CHARGING` 明确失败 | 失败 | **保持 `STOPPING`**（不能假定已停止） | `FAULT`，等待恢复处理 |
| 超时 / 网络错误 | — | 继续重试，**不得伪造设备失败回执** | 维持 |

其余冻结项：

- 乱序回执（例如 `CHARGE_STOPPED` 早于 `CHARGE_STARTED` 到达）→ **返回 409 并记录审计**，不做暂存；
- 回执幂等、状态推进与 Outbox 写入必须在**同一事务**内完成；
- `CHARGE_STARTED` / `CHARGE_STOPPED` 事件仍由该事务产生，B 线 Worker 只按通知消费（BE-I-01 已冻结的语义不变）。

### 4. STOP 失败后的恢复路径（已冻结）

保持 `STOPPING` 是安全的（不能假定设备已停止），但"等待恢复处理"本身不是机制：没有恢复入口的订单会永久占用
用户与充电桩。因此本模块必须交付一条**有上限、有退避、有人工出口**的恢复路径：

- 由后台恢复任务（A 线 janitor，与既有 `ExpireStaleOrders` / `ReleaseOrphanedChargers` 同一循环）扫描
  "订单停在 `STOPPING` 且充电桩为 `FAULT`"的订单，以**新的 `command_id`** 重新下发 `STOP_CHARGING`；
- **次数上限 + 退避**：重试次数依据该订单已产生的 `STOP_CHARGING` 命令记录（outbox/审计中已有，无需新表），
  超过上限即停止重发；
- 超过上限后：保持订单 `STOPPING`、充电桩 `FAULT`，并**进入人工告警**；
- **绝不自动释放充电桩，绝不自动结算**：设备可能仍在充电，任何自动收尾都会产生错误账单或把故障设备放回
  可分配状态；
- 恢复任务必须可观测：每次重发与进入人工告警都要有日志与计数（复用既有 janitor 的日志风格）。

## 变更范围与所有权

| 内容 | 归属 | 说明 |
|---|---|---|
| `backend/internal/order/`（`ConfirmStart`/`ConfirmStop` 及其事务） | A 线 | 已存在；本模块只增加调用入口，不改状态机规则 |
| `backend/internal/httpapi/`、`backend/cmd/api/` | A 线 | 新端点的注册与处理器 |
| `api/openapi.yaml`（仓库根目录） | 集成人员 | 先登记再实现 |
| `backend/internal/repository/postgres/` | A 线 | 回执所需的校验查询（如订单-充电桩归属） |
| `backend/internal/worker/`（动作集、回执消费）、`backend/cmd/worker/`、`backend/cmd/mock-gateway/` | B 线 | 动作扩展与回执消费 |
| `backend/scripts/verify-closed-loop.sh` | B 线 | 扩展为完整订单状态链路 |

## 契约和数据

- **新增 API**：`POST /api/v1/internal/charger-events`（内部、HTTPS + Bearer 服务令牌）。响应体沿用统一错误包络；
  成功返回订单的新状态与已应用的 `eventId`，便于设备侧重试判定；同 ID 不同载荷返回 409，乱序回执返回 409。
- **PostgreSQL 迁移**：**新增 `0007_charger_command_outcomes.sql`（up/down 齐备，不改历史迁移）**。
  第 3 版原写"预计不需要新表"，第二轮审查（`b7f8c11`）指出命令结果的去重不能复用 `idempotency_records`
  ——它的记录 24 小时后过期，而 `STOPPING` 订单在 24 小时后仍然有业务意义（正是 STOP 恢复任务处理的状态），
  过期后重复的 STOP 结果会被当成首次交付再次应用。按本审批单原有约定（先改本审批单，再加顺序迁移文件），
  此处登记新表：`charger_command_outcomes(command_id PK, charger_id, order_no, action, result, applied,
  recorded_at)`，`result`/`action` 用 CHECK 约束到冻结枚举，**不设过期列**，与 outbox 行一样长期保留。
  down 脚本位于 `backend/migrations/down/`（不参与嵌入迁移集）。
- **Redis Key/Stream**：不新增、不改名。回执产生的仍是既有 `CHARGE_STARTED` / `CHARGE_STOPPED` 事件，
  落在既有 `ncs:stream:charge-event`。
- **幂等与并发**：`idempotency_records`（key=`eventId`、`request_hash`=载荷摘要）+ 订单行锁 + 状态机校验，
  三者必须在同一事务；重复回执不得重复计费或重复推进状态。乱序回执按状态机拒绝（409）并记录审计。

## 审查修复记录（第 4 版）

三项 CHANGES_REQUIRED 均已修复，每项都补了回归用例并做了反向验证（先破坏被保护的行为，确认用例失败）。

### 1. 命令结果按 `command_id` 事务内幂等

```text
问题：RecordChargerCommandResult 没有按 command_id 去重。STOP_CHARGING 的 FAILED 结果**有意不改变**订单
      STOPPING 状态，因此重放时仍会判定 applied=true 并再次写入 CHARGER_COMMAND_COMPLETED。

修复：internal/repository/postgres/orders.go
      - 新增 chargerCommandResultScope + commandResultDigest（chargerID|orderNo|action|result）；
      - 在**同一事务**内先 claimIdempotency(key=command_id)，成功后 apply + 写事件 + finalize；
      - 已 SUCCEEDED 的重放直接返回 applied=false，不写事件；并发中的重复交付返回
        ErrIdempotencyInProgress（可重试），重试后走重放分支；
      - 结果"未改变任何状态"（如命令被接受、或订单已越过该命令）也照样 finalize：命令已被回答，
        后续交付必须被识别为同一结论，而不是重新判定一次；
      - 同一 command_id 给出**不同**结论 → ErrIdempotencyConflict，不覆盖首次结论。

用例：TestChargerCommandResultIsRecordedOncePerCommand（真实 PostgreSQL）
      —— 首次 applied=true 且事件 1 条；重放 applied=false、事件仍 1 条；不同结论 → 冲突、事件仍 1 条
用例：TestConcurrentChargerCommandResultIsAppliedOnce（真实 PostgreSQL，4 个并发交付）
      —— applied=true 恰好 1 次、CHARGER_COMMAND_COMPLETED 恰好 1 条

反向验证：
  移除 claim（recorded := false）→ 重放用例失败（"a replay must not report a second application"），
  并发用例失败（"4 deliveries applied the same outcome, want exactly 1"）——正是审查复现的缺陷形状。

设计说明：并发用例刻意走 STOP_CHARGING FAILED 路径，而不是 RESTART。RESTART 的充电桩更新第二次会匹配
  0 行并返回 applied=false，即使没有 claim 也"看起来正确"，用它写并发用例根本覆盖不到这个缺陷。
```

### 2. 只有明确的成功/失败枚举才是设备结论

```text
问题：deviceOutcome 原样透传 TIMED_OUT 或任意未知 status/result，而 RecordChargerCommandResult 把所有
      非 COMPLETED 结果当成明确失败 —— START 超时会错误地把订单置 FAILED、STOP 超时会错误地把充电桩置 FAULT，
      违反"超时继续重试、不得伪造失败结果"。

修复：internal/order/service.go
      - 冻结结论枚举：CommandResultCompleted / CommandResultFailed + IsCommandResult()；
      cmd/worker/adapters.go
      - deviceOutcome 只接受两个枚举（兼容 SUCCESS/SUCCEEDED/OK 与 FAILURE/ERROR/REJECTED、accepted 布尔）；
      - 无结论状态（TIMED_OUT/TIMEOUT/PENDING/ACCEPTED/IN_PROGRESS/PROCESSING/QUEUED/RETRYING）→ 可重试错误，
        不落任何设备结果；
      - 未知状态 → 永久错误（协议违约，交给死信与运维，而不是猜）；
      - status 是结论来源，result 仅在 status 缺失时兜底，避免 status=TIMED_OUT + result=FAILED 被读成失败；
      - Dispatch 不再把 deviceOutcome 的错误一律标成 permanent，改为保留它自带的分类。
      internal/repository/postgres/orders.go
      - RecordChargerCommandResult 只接受冻结枚举，其余值直接拒绝（不再"非 COMPLETED 即失败"）；
      - 充电桩状态判定改用枚举常量。

用例：TestDispatchDoesNotRecordAVerdictlessGatewayAnswer（真实 HTTP 处理器）
      —— TIMED_OUT / IN_PROGRESS / ACCEPTED → 可重试且不记录；REBOOTING → 永久且不记录；
         FAILED / COMPLETED → 各记录 1 条
用例：TestDeviceOutcomeMapping（枚举、无结论、未知、status 优先于 result、accepted 布尔）
用例：TestChargerCommandResultRefusesAnOutcomeOutsideTheFrozenEnum（真实 PostgreSQL）
      —— TIMED_OUT / SOMETHING_ELSE / "" / ACCEPTED 全部被拒绝，充电桩保持 RESTARTING（绝不回到 IDLE）、
         不产生完成事件

反向验证：
  恢复 `default: return status, nil` → TestDeviceOutcomeMapping 与
  TestDispatchDoesNotRecordAVerdictlessGatewayAnswer 的 4 个子用例全部失败（"must not be dispatched as a
  settled outcome"、"deviceOutcome("TIMED_OUT") = "TIMED_OUT""）；
  移除仓储层的枚举校验 → "result "TIMED_OUT" was accepted as a device outcome"。

测试修正说明：原 TestChargerCommandResultDecidesTheChargerStatus 里"TIMED_OUT 把充电桩置 FAULT"、
  "未知结论把充电桩置 FAULT"两行**正是本项缺陷的编码**，已改为"拒绝该结论且充电桩维持 RESTARTING"。
  保护意图未变且更强：未知结论既不会回到 IDLE，也不会被当成设备故障。
```

### 3. 模拟网关的请求指纹包含 `order_no`

```text
问题：新增 charge action 后，command_id 对应的请求身份只比较 charger_id + action。同一 command_id、同一
      charger、同一 action 从 ORD-A 改成 ORD-B，第二次仍返回首次结果，可能把设备结论关联到错误订单。

修复：cmd/mock-gateway/main.go
      - commandRecord 增加 orderNo；claim() 的身份比较加入 orderNo；simulate() 补写 orderNo；
      - handleCommand 的注释同步说明"id/charger/action/order 四者相同才是同一请求"。

用例：TestGatewayRejectsAReusedCommandIDForADifferentOrder
      —— 同 id/charger/action、order 由 ORD-A 改 ORD-B → 409，且设备工作只执行 1 次；同 order 重试仍 200
用例：TestGatewayStillReplaysACommandWithoutAnOrder
      —— 站点级 RESTART 不带 order_no，两次相同请求仍是重放（attempts=2）而不是冲突

反向验证：
  移除 claim 中的 orderNo 比较 → status = 200（"attempts":2）而不是 409 —— 与审查的实测复现完全一致。
```

### 第 5 版验证

```text
命令：gofmt -l ./cmd ./internal → 无输出
命令：go build ./... && go vet ./... → 通过
命令：NCS_TEST_PG_DSN=... NCS_REDIS_TEST_DB=14 go test -count=1 -race ./...
结果：全部 ok（见下"第 5 版交付前全量检查"）
命令：schema_migrations → 1..7；charger_command_outcomes 已由同一 runner 在空库/既有库上建立
命令：NCS_E2E_ALLOW_DESTRUCTIVE=true ... bash backend/scripts/verify-closed-loop.sh
结果：PASS —— CREATED -> STARTING -> CHARGING -> STOPPING -> COMPLETED 全链路仍成立
```

### 第 4 版验证

```text
命令：gofmt -l ./cmd ./internal → 无输出
命令：go build ./... && go vet ./... → 通过
命令：NCS_TEST_PG_DSN=... NCS_REDIS_TEST_DB=14 go test -count=1 -race ./...
结果：全部 ok（cmd/mock-gateway 1.975s、cmd/outbox-publisher 1.616s、cmd/worker 1.946s、admin 1.544s、
      auth 3.748s、config 1.012s、event 1.013s、httpapi 1.012s、observability 1.091s、order 1.554s、
      repository/postgres 5.576s、repository/redis 3.430s、station 1.490s、worker 1.118s）
命令：internal/repository/postgres 连续两次运行（命令结果幂等记录跨运行保留 24 小时）
结果：两次均 ok —— 新用例的命令号按运行唯一，不会重放上一次运行留下的结论
命令：NCS_E2E_ALLOW_DESTRUCTIVE=true ... bash backend/scripts/verify-closed-loop.sh
结果：PASS —— CREATED -> STARTING -> CHARGING -> STOPPING -> COMPLETED 全链路、事实时间与低价窗口、
      重复回执不重复计费、同 id 不同事实 409、无令牌 401、pending=0、无死信
```

### 第 4 版遗留项（第二轮审查 P1）：命令结果去重必须与幂等窗口无关

```text
问题：第 4 版把命令结果去重放在 idempotency_records（24 小时窗口）里，并披露为"已知取舍"。该取舍是错的：
      STOPPING 订单在 24 小时后仍然有业务意义 —— 那正是 STOP 失败恢复任务扫描并重发命令的状态；窗口过期后
      重复的 STOP 结果会被当成首次交付再次判定 applied=true 并写出第二条 CHARGER_COMMAND_COMPLETED。
      "极旧结果已无意义"不成立：失效的是幂等缓存，不是设备事实。

修复：
      - 新增顺序迁移 0007（up/down）：charger_command_outcomes(command_id PK, charger_id, order_no, action,
        result, applied, recorded_at)，result/action 由 CHECK 约束到冻结枚举，**没有过期列**；
      - orders.go 的 claim 改为 claimCommandOutcome：在同一事务内 INSERT ... ON CONFLICT (command_id)
        DO NOTHING 抢占该命令的结果槽位，抢占成功才应用并写事件；已存在且各字段一致 → 重放（applied=false、
        不写事件）；各字段不一致 → ErrIdempotencyConflict；并发交付在冲突上阻塞后读到已提交行，回到重放分支；
      - applied 标记在同一事务内回写，记录"这条命令的设备结论是否改变过状态"。

用例：TestChargerCommandResultSurvivesTheIdempotencyWindow（真实 PostgreSQL）
      —— 记录 STOP 失败后把 store 时钟推进 48 小时（任何 24 小时窗口都已过期），重放仍 applied=false、
         完成事件仍 1 条；并断言结果行仍在且 applied=true、订单仍 STOPPING、充电桩仍 FAULT
用例：TestChargerCommandResultIsRecordedOncePerCommand / TestConcurrentChargerCommandResultIsAppliedOnce
      —— 顺序重放与 4 路并发都只产生一次应用、一条事件（并发用例断言每个交付都必须被正常回答，不再容忍
         "in progress" 分支：新实现里该分支已不可达，任何错误都应当暴露）
用例：TestChargerCommandOutcomeTableIsDurable（结构不变量）
      —— 0007 必须以 command_id 为主键、CHECK 约束到冻结枚举，且**不含任何过期列**（expires/expires_at/ttl）

反向验证：把 claim 换回第 4 版的 idempotency_records + 24 小时窗口实现 →
      TestChargerCommandResultSurvivesTheIdempotencyWindow 失败：
      "a duplicate STOP result was applied again after the idempotency window" —— 与审查结论一致。

保留策略：charger_command_outcomes 不设过期、不清理，与 outbox_events 相同量级（每条设备命令一行）。
      它同时是"哪条命令产生了订单当前状态"的可审计记录，运维与恢复任务都需要它长期可查。

回执端点为什么可以继续用有界的幂等窗口：`ConfirmStart`/`ConfirmStop` 的幂等记录仍是 24 小时，但状态机是它
      背后的持久防线 —— 已应用的回执会把订单推进到 CHARGING/COMPLETED，窗口过期后的重复回执会被判为非法跃迁
      （409）并写审计，而不是被再次应用。命令结果没有这道防线（STOP 失败**有意**保持 STOPPING），因此两者的
      去重记录保留策略不同，这是刻意的，不是遗漏。
```


### 第 5 版复审结论（`a25e028`）

第 4 版的三项修复均通过代码复核：同事务命令结果 claim/finalize、无结论状态不落库、模拟网关指纹包含
`order_no`。真实 PostgreSQL + Redis 下 `gofmt`、`go build ./...`、`go vet ./...`、
`go test -count=1 -race ./...` 与完整订单闭环脚本均通过。

仍有一项阻断：上述 24 小时 TTL 不能保证命令结果的持久去重。`STOP_CHARGING` 明确失败后订单按契约
保持 `STOPPING`、充电桩保持 `FAULT`；若结果事务已提交、但 Worker 尚未将原 Stream 消费记录完成并 ACK，
超过 24 小时的进程/依赖故障后，Pending 恢复会重新交付原命令。`claimIdempotency` 会删除过期记录并
重新 claim，`applyChargeCommandOutcome` 仍会把同一 FAILED 结果判为 applied=true，再追加一条新的
`CHARGER_COMMAND_COMPLETED`。重试预算只限制失败交付次数，不限制停机后的恢复时间，且 `STOPPING`
状态在 24 小时后仍有业务意义。不同结论在过期后也不再被识别为冲突。

修复门槛：命令结果需有跨恢复窗口的持久去重/冲突判定；可以复用现有表或 Outbox 的命令记录，不要求
新增永久去重表，但不得依赖统一 24 小时 TTL。增加真实 PostgreSQL 测试，模拟过期后同一
`STOP_CHARGING FAILED` 结果重放，断言完成事件仍恰好 1 条、不同结论仍为冲突。

### 最终复审（`ec5e035`）

`charger_command_outcomes` 以 `command_id` 为主键，无过期列，且设备结果、订单/充电桩变更和
`CHARGER_COMMAND_COMPLETED` 在同一事务中提交。重复结果返回 no-op，不同结果返回冲突。
真实 PostgreSQL 用例通过 48 小时时钟推进验证 STOP 失败结果不再重复应用，完成事件仍恰好一条；
并发、结果枚举与迁移结构测试通过。测试库实际已应用迁移版本 1～7。`gofmt`、`go build ./...`、
`go vet ./...`、真实 PostgreSQL/Redis 下 `go test -count=1 -race ./...` 和四进程闭环脚本均通过。

部署门槛：先应用 0007，再启动使用 `RecordChargerCommandResult` 的新 Worker；新端点仍需
`NCS_CHARGER_GATEWAY_TOKEN`、HTTPS 与 Nginx 网关来源网络限制。审批不等于已合并或已验证真实设备协议。

## 验证记录（第 3 版：实现与验证）

```text
状态：两项交付均已实现并验证；回执端点的 OpenAPI 登记仍待集成人员执行（第 4.1 节"先登记再实现"），
      因此本模块在登记落地前不请求合并。
实现提交（分支 codex/migration/i-02-charger-control-receipts）：
      57b884e 命令载荷与产生路径（B 线 + A 线订单事务）
      fa44e40 回执端点、事实时间、失败语义与 STOP 恢复
      3fad635 / b610c44 / 14382f8 契约测试、真实 PostgreSQL 用例、闭环脚本
```

### 交付前的全量检查

```text
环境：NCS_TEST_PG_DSN=postgres://ncs_test@127.0.0.1:55439/ncs_a03?sslmode=disable
      NCS_REDIS_TEST_DB=14（真实 PostgreSQL 16 与 Redis 7）

命令：gofmt -l ./cmd ./internal
结果：无输出

命令：go build ./...
结果：通过

命令：go vet ./...
结果：通过，无告警

命令：go test -count=1 -race ./...
结果：全部 ok —— cmd/mock-gateway 2.006s、cmd/outbox-publisher 1.623s、cmd/worker 1.713s、
      admin 1.551s、auth 3.554s、config 1.012s、event 1.015s、httpapi 1.013s、
      observability 1.087s、order 1.562s、repository/postgres 5.370s、repository/redis 3.423s、
      station 1.547s、worker 1.121s
```

### 一项 1：设备回执端点（`POST /api/v1/internal/charger-events`）

```text
实现：internal/order/charger_events.go（处理器与鉴权）、internal/order/types.go（事实时间校验、
      回执命令字段）、internal/repository/postgres/orders.go（ConfirmStart / ConfirmStop 事务）、
      internal/config/config.go（令牌与未来偏差配置）、cmd/api/main.go（注册与拒绝启动）

鉴权：Authorization: Bearer <NCS_CHARGER_GATEWAY_TOKEN>，常量时间比较；令牌无默认值，
      未配置时 cmd/api 拒绝启动（见下表"未配置令牌"用例）。

单元用例（internal/order/charger_events_test.go，真实 HTTP 处理器 + 真实路由）：
  缺 Authorization / Basic / 错误令牌 / 空 Bearer / 令牌放在别的头 → 401；Bearer 大小写不敏感 → 200；
  未知 eventType、缺 eventId、缺 occurredAt、缺 chargerId、chargerId 非数字、STOPPED 缺 energyWh、
  负 energyWh、非 JSON → 400；ErrOrderNotFound → 404；乱序 / 归属不符 / 状态不允许 → 409；
  数据库不可用 → 503；GET → 405；未知子路径 → 404
```

**真实 PostgreSQL 用例**（`internal/repository/postgres/charger_receipt_integration_test.go`）：

```text
用例：TestChargerReceiptWritesTheDeviceFactTime
结果：started_at / stopped_at / completed_at 均等于回执携带的事实时间（毫秒级相等断言）

用例：TestChargerReceiptIsIdempotent
结果：同 eventId 同载荷第二次调用返回首次结果（状态与订单号一致），CHARGE_STARTED 事件恰好 1 条；
      同 eventId 不同摘要 → ErrIdempotencyConflict（409）

用例：TestChargerReceiptRejectionsAreAuditedAndChangeNothing
结果：STARTING 状态下收到 CHARGE_STOPPED → ErrFactTimeOutOfOrder，订单仍为 STARTING，
      order_events 写入 CHARGER_EVENT_REJECTED；充电桩归属不符 → ErrChargerOrderMismatch 且同样审计；
      订单不存在 → ErrOrderNotFound；被拒绝的 eventId 之后仍可正常应用（拒绝不占用幂等键）

用例：TestChargerReceiptRejectsAStopBeforeTheRecordedStart
结果：停止事实时间早于 started_at → ErrFactTimeOutOfOrder，订单保持 STOPPING，审计 1 条
```

### 一项 2：命令动作集与命令产生路径

```text
实现：worker.SupportedCommandActions() = RESTART / START_CHARGING / STOP_CHARGING；
      ChargeCommandActions() + RequiresOrderNo()；命令载荷新增 order_no（START/STOP 必填）；
      order 事务在**同一事务内**追加 CHARGER_COMMAND_REQUESTED（携带 order_no 与动作），
      生命周期通知事件照旧保留；Dispatcher 与模拟网关同步扩展。

用例：TestStartAndStopQueueTheDeviceCommandInTheSameTransaction（真实 PostgreSQL）
结果：STARTING 与 STOPPING 各产生 1 条 CHARGER_COMMAND_REQUESTED，携带 order_no / charger_id /
      新 command_id；两种动作的 command_id 不同；下单不产生命令事件
用例：TestDispatchDoesNotFabricateAFailureForAChargeCommandTimeout（B 线，START/STOP 各一子例）
结果：超时保持可重试，且不记录任何设备结果（不伪造失败回执、不失败订单、不置 FAULT）
用例：TestCommandRequestHandlerDispatchesAChargeCommandWithItsOrder /
      TestCommandRequestHandlerRejectsAChargeCommandWithoutAnOrder（B 线解析）
用例：TestDispatchCarriesTheOrderNumberForAChargeCommand / TestDispatchRejectsAChargeCommandWithoutAnOrder
用例：TestGatewayAcceptsAChargeCommandWithItsOrder /
      TestGatewayRejectsAChargeCommandWithoutAnOrderTwiceWithoutLeavingAPlaceholder（模拟网关）
```

### 三项与四项：失败语义、STOP 恢复路径

```text
用例：TestChargeCommandFailureSemantics（真实 PostgreSQL，4 个子用例）
结果：命令被接受（COMPLETED）→ 订单不推进、充电桩维持 OCCUPIED、不产生完成事件；
      START 明确失败 → 订单 FAILED + 充电桩 FAULT + CHARGER_COMMAND_COMPLETED(result=FAILED)；
      STOP 明确失败 → 订单保持 STOPPING + 充电桩 FAULT，energy_wh 仍为 0、payment 未 PAID；
      迟到的 START 失败（订单已 CHARGING）→ 不应用，运行中的充电不被失败

用例：TestStopRecoveryReissuesBoundedCommandsAndEscalates / TestStopRecoveryEndsWithAReceipt
结果：退避窗口内不重发（仍只有 1 条 STOP_CHARGING）；超过退避后以**新的 command_id** 重发；
      达到次数上限后不再重发、写入 1 条 STOP_RECOVERY_ESCALATED（重复扫描不重复写）；
      订单保持 STOPPING、充电桩保持 FAULT、energy_wh=0、paid_cents=0、payment != PAID
      （断言"绝不自动释放、绝不自动结算"）；设备最终回执后订单正常 COMPLETED 且充电桩 IDLE
```

### 事实时间决定分时计费（冻结项 9）

```text
用例：TestChargerReceiptFactTimeDrivesTimeOfUseBilling（真实 PostgreSQL）
结果：同样 1 kWh，事实时间落在 00:00-08:00 UTC 低价窗口时账单 50 分，落在 12:00-13:00 峰段时 120 分
      ——事实时间确实决定计费窗口，而不是服务器时间

排查中发现的真实缺陷：ConfirmStop 原先把**数据库会话时区**下的 started_at 直接交给
      SplitChargeSegments / EffectiveElectricityPrice（按 time.Hour() 判断峰谷）。本实例会话时区为
      Asia/Shanghai（+08），03:00 UTC 的充电会被当作 11:00 计费，落在完全不同的窗口。
      已改为两端都显式 UTC（orders.go ConfirmStop），并在闭环脚本中用真实回执断言低价窗口生效。

后续修正（峰谷窗口按哪个时区）：把两端固定成 UTC 只消除了"会话时区"这个隐式输入，
      但运营配置的窗口本来就是**本地墙上时间**（seeds/dev_seed.sql 里是 23:00-07:00）。
      按 UTC 判定等于把窗口挪到本地 07:00-15:00：实测本地 14:15（06:15 UTC）的充电拿到了
      谷时价 60（BILL_DETAIL 分段 price=60），而真正的本地夜间谷时反而按峰价计。
      现改为按业务时区判定：`NCS_BILLING_TZ`（默认 Asia/Shanghai，见
      `order.DefaultBillingLocation()` 与 `postgres.WithBillingLocation`），
      **计费时刻本身仍是 UTC**，只有窗口查表用本地小时。
      用例：TestChargerReceiptFactTimeDrivesTimeOfUseBilling（窗口 23:00-07:00 本地，
      12:00 本地必须按峰价）、TestTariffWindowFollowsTheBillingLocation、
      TestOrderDetailExposesEstimateBasisWhileCharging。
```

### 真实 PostgreSQL + Redis 全链路（验收计划第 4 项）

`backend/scripts/verify-closed-loop.sh` 现在启动**四个真实进程**（`cmd/api` 带网关服务令牌、
`cmd/outbox-publisher`、`cmd/worker`、`cmd/mock-gateway`），并经内部回执端点驱动完整订单链路：

```text
命令：NCS_E2E_ALLOW_DESTRUCTIVE=true NCS_E2E_ALLOWED_DATABASE=ncs_a03 NCS_POSTGRES_DSN=...       NCS_REDIS_ADDR=127.0.0.1:6379 NCS_REDIS_DB=14 bash backend/scripts/verify-closed-loop.sh
结果：PASS（关键输出，连续两次运行均通过）

  order charger: 29, command charger: 30, failing charger: 1
  charger 29 released after the device confirmed the revocation
  order created for the receipt chain: ORD20260915031510919dcbb9
  the device accepted START_CHARGING and the order is still STARTING
  CHARGE_STARTED applied: status=CHARGING, started_at=2026-09-15T01:10:00Z (the device's own fact time)
  the device accepted STOP_CHARGING and the order is still STOPPING
  CHARGE_STOPPED applied: status=COMPLETED, stopped_at=2026-09-15T01:40:00Z, energy=1000Wh,
      amount=110 cents (off-peak 60 + service 50), charger=IDLE
  order chain verified with real receipts: CREATED -> STARTING -> CHARGING -> STOPPING -> COMPLETED
      (6 event types consumed)
  charger 1: event result=FAILED, status=FAULT (not IDLE), completion consumed
  stream order-event / charge-event / charger-command: 0 pending
  second publisher stood by; the advisory lock kept publishing single-active
  entry reclaimed and acknowledged after the crash: records=1, pending=0
  publisher batches: 12 / worker consumed: 18
  PASS: verified with real processes against real PostgreSQL and Redis

同时断言的否定项：未带服务令牌的回执 → 401；同 eventId 不同载荷 → 409；重复回执不产生第二条
CHARGE_STARTED，也不改变账单；被接受的命令不产生 CHARGER_COMMAND_COMPLETED，也不改变订单状态；
死信流长度为 0；本次运行涉及的消费记录不得出现 FAILED / DEAD_LETTERING / DEAD_LETTERED；
完成订单的 payment_status 保持 PENDING（平台不自动扣款）。
脚本 PASS 文本已删除"不验证 CHARGING/COMPLETED"的免责声明。
```

### 反向验证（每条回归断言都先破坏它所保护的行为）

```text
1) 端点鉴权：把 authorized 判断短路 → 6 个未授权用例全部返回 200，用例失败
2) occurredAt 必须为 UTC：删除偏移量检查 → 带 +08:00 偏移的历史时刻被接受，用例失败
   （该用例最初用"8 小时后的时刻"构造，未来偏差检查先一步拒绝，反向验证才发现它没有真正覆盖 UTC 规则，
     已改为"过去的时刻 + 偏移量"）
3) 事实时间写库：started_at 改回 CURRENT_TIMESTAMP → 断言得到服务器时间（与事实时间差 30 分钟），用例失败
4) 事实时间计费：计费终点改回服务器时钟 → 峰段充电被按低价结算（120 分变 50 分），用例失败
5) 回执幂等：关闭 claimReceipt → 重复回执被当成非法状态跃迁（CHARGING -> CHARGING）返回错误而不是重放，用例失败
6) 失败语义：订单不再置 FAILED、充电桩不再置 FAULT → 两个子用例分别失败
7) 恢复次数上限与退避：两处判断改为 false → 退避窗口内立即重发，用例失败
8) 命令动作契约（B 线）：移除 order_no 校验 → 缺失订单号的载荷被派发；从载荷里去掉 order_no →
   网关收不到订单号，用例失败
9) 模拟网关：移除拒绝路径上的 abandon → 第二次请求在限定时间内超时（"context deadline exceeded"），
   证明占位符回收是必需的
9.1) 超时不得伪造失败回执：在 Dispatcher 的传输错误分支插入"记录 FAILED 结果" → 新增用例
   TestDispatchDoesNotFabricateAFailureForAChargeCommandTimeout 对 START_CHARGING 与 STOP_CHARGING
   两个动作都失败（"a timeout recorded a device outcome"），证明超时确实不写任何设备结果
   （即不会让订单 FAILED、也不会把充电桩置 FAULT）
10) 闭环脚本的仓库断言：把 started_at / amount 的断言指向错误值（人工核对）——脚本按预期失败并打印实际值
```

### 差异检查（验收计划第 11 项）

```text
go.mod / go.sum：未改动（无新增依赖）
api/openapi.yaml：集成人员已在 `052270e` 登记 `POST /api/v1/internal/charger-events`、独立网关服务令牌
      security scheme、请求/响应结构及 400/401/404/409/503 响应
approval-log.md：未改动（由审批人在合并门更新）
其它线的工作区：未触碰
部署可见变更：cmd/api 新增必需环境变量 NCS_CHARGER_GATEWAY_TOKEN（无默认值，缺失即拒绝启动），
      以及可选 NCS_CHARGER_EVENT_MAX_FUTURE_SKEW（默认 5m）、NCS_STOP_RECOVERY_MAX_ATTEMPTS（默认 3）、
      NCS_STOP_RECOVERY_BACKOFF（默认 5m）；闭环脚本已按此设置。
```

### 验收计划逐项对照

```text
1) 全量检查：见上（gofmt / build / vet / go test -race，真实 PostgreSQL + Redis）——通过
2) 回执端点成功 / 重复 / 订单不存在 / 归属不符 / 状态不允许：单元 8 类 + 真实 PostgreSQL 4 个用例——通过
3) 三种动作的成功与失败路径：解析、Dispatcher、模拟网关、真实 PostgreSQL——通过
4) 完整订单链路：闭环脚本以真实回执驱动 CREATED -> STARTING -> CHARGING -> STOPPING -> COMPLETED——通过
5) 乱序（409 + 审计）与重复（同载荷首次结果、不同载荷 409）：真实 PostgreSQL 用例 + 闭环脚本——通过
6) 鉴权：缺令牌 / 错令牌 / 正确令牌 / 未配置令牌时拒绝启动——通过
7) 命令产生路径：同事务内产生 CHARGER_COMMAND_REQUESTED（含 order_no），生命周期事件保留——通过
8) 失败语义：START 失败 -> FAILED + FAULT；STOP 失败 -> 保持 STOPPING + FAULT；超时不伪造失败——通过
9) 时间语义：事实时间写 started_at / stopped_at 并参与分时计费；非 UTC / 停止早于开始 / 未来偏差——通过
10) STOP 恢复路径：新 command_id、次数上限、退避、一次性告警，且不自动释放/结算——通过
11) 差异检查：见上——OpenAPI 登记已由集成人员完成；approval-log 仍由审批人在最终合并门更新
```

验收计划（开工前即冻结）：

```text
1) gofmt / go build / go vet / go test -race ./...（真实 PostgreSQL + Redis）
2) 回执端点：成功、重复（幂等）、订单不存在、订单-充电桩不匹配、状态不允许，各一条真实用例
3) 命令动作：RESTART / START_CHARGING / STOP_CHARGING 各自的成功与失败路径（含未知充电桩、
   动作与充电桩状态不匹配）
4) 完整订单状态链路：CREATED -> STARTING -> CHARGING -> STOPPING -> COMPLETED（由真实回执驱动）
   并在 PostgreSQL 中回写；闭环脚本据此去掉"不验证 CHARGING/COMPLETED"的免责声明
5) 乱序回执（409 + 审计）与重复回执（同载荷返回首次结果、不同载荷 409）：不重复计费、不重复推进状态
6) 鉴权：缺少令牌、错误令牌、正确令牌；未配置 NCS_CHARGER_GATEWAY_TOKEN 时进程拒绝启动
7) 命令产生路径：StartCharging / StopCharging 在**同一事务**内产生 CHARGER_COMMAND_REQUESTED
   （携带 START_CHARGING / STOP_CHARGING 与 order_no），原有生命周期通知事件同时保留
8) 失败语义：START 失败 -> 订单 FAILED + 充电桩 FAULT；STOP 失败 -> 订单保持 STOPPING + 充电桩 FAULT；
   超时重试且不产生伪造失败回执
9) 时间语义：`occurredAt` 写入 `started_at` / `stopped_at` 并参与分时计费；非 UTC、停止早于开始、
   未来偏差超过 5 分钟分别 400/409/400；重复回执返回首次计算结果
10) STOP 恢复路径：恢复任务以新 command_id 重发 STOP_CHARGING（有次数上限与退避），达到上限后保持
    STOPPING/FAULT 并告警；断言**没有**自动释放充电桩、**没有**自动结算
11) 差异检查：共享文件（go.mod/go.sum/api/openapi.yaml/approval-log.md）按归属规则处理
```

## 风险和回滚

- 已知风险：
  1. **内部端点是新的攻击面**：必须以服务身份鉴权、限流，并校验订单与充电桩的归属，否则伪造回执可以
     推进任意订单的状态；
  2. **动作集扩展会影响两端**：动作取值同时出现在 OpenAPI、事件契约、Dispatcher 与模拟器，任何一处
     漏改都会造成"平台发了设备听不懂的命令"或"设备回执无对应动作"；契约测试必须覆盖全集；
  3. **乱序回执**：设备可能先报停止再报开始（重连、离线补报）。已冻结为**拒绝（409）+ 审计**、不暂存，
     因此网关侧需要保证补报顺序（或由运维按审计修正），这一点要在网关联调时验证；
  4. **能量与电表读数的可信度**：`energyWh` 直接进入计费，`ConfirmStop` 已校验与电表读数一致，回执端点
     必须保证两者要么都提供、要么提供可核查的来源；
  5. **命令与回执都依赖网关实现质量**：命令成功只代表设备接受，状态推进完全依赖回执，因此网关必须保证
     回执的可靠投递，否则订单会长期停在 `STARTING` / `STOPPING`；
  6. **恢复任务的重发上限**：上限过低会让可恢复订单过早转入人工，过高会持续向故障设备发命令。首版取保守值
     （次数少、退避明显），每次重发与告警都记入日志以便调参；
  7. **事实时间可被伪造**：`occurredAt` 决定计费窗口，因此它必须与鉴权、订单-充电桩归属校验一起生效；
     未来偏差阈值只拦截明显异常，真正的可信边界是"只有网关能调用这个端点"。
- 回滚方式：新增端点为独立路由，回滚时先停止调用方（设备/网关），再回退到上一部署版本；动作集扩展若已
  下发，需同步回退 Dispatcher 与模拟器，避免两端动作集不一致。不涉及 Redis/Stream 契约变更，因此不需要
  数据迁移回滚。
- 是否影响旧 C++ 系统：否。

## 审批结论

```text
状态：APPROVED
审批人：Codex
审批时间：2026-09-15
第 4 版三项修改要求：已复审通过。
第 5 版剩余修改要求：已由不设过期的 charger_command_outcomes 与真实 PG 跨日重放测试满足。
批准范围：设备控制动作、内部回执入口、订单状态与事实时间计费、命令失败语义、STOP 恢复与
  事件/设备结果持久去重闭环；验证出口是 mock gateway，不代表真实设备协议已联调。
合并门：需先应用顺序迁移 0007，并由集成人员更新 approval-log.md。
```
