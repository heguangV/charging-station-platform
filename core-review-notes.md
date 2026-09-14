# core/ 审核导读（本地笔记，未跟踪文件，请勿提交）

> 用途：配合人工审核 `core/` 使用，逐文件给出"文件功能 + 类/函数说明 + 审读提示"。
> 代码本身已带注释（文件头注释 = 文件功能；头文件函数上方注释 = 契约；cpp 内注释 = 实现层坑点），
> 本文档是其展开与索引。行号基于 2026-09-09 工作区，代码变更后会漂移，仅作定位参考。
>
> **更新说明（2026-09-09 晚）**：工作区在会话期间新增了"订单确认与申诉"特性
> （结算→100 待用户确认→用户确认扣款 / 申诉 110→管理端核准取消），涉及
> charging_repository、charge_flow_service、infrastructure/sqlite（v10 迁移）、
> server/controller/order_payment_routes 等。本文档相关段落已同步为当前工作区状态。

---

## 0. 模块地图

```
core/
├── include/ncs/core/{error.h, result.h}   Qt 客户端错误模型（ncs_core 静态库，链接 Qt6::Core）
├── src/error.cpp                           errorCodeName 的 QString 实现
├── domain/error_code.h                     服务端权威错误码契约（ncs_core_domain，零依赖 INTERFACE）
└── application/                            业务主体（ncs_core_application，C++17，无 Qt）
    ├── 端口与适配器：charging_repository / admin_repository(+in_memory×2)
    │                 user_account_repository(+in_memory) / business_numbers / readiness_probe
    │                 （Geocoder、RoutePlanner、MlTaskLauncher、AnalyticsRepository、
    │                   ModelArtifactStore、OrderReviewRepository 内嵌在各服务头文件中）
    ├── 横切组件：session_manager / event_hub / idempotency_service / bounded_executor
    │              security_crypto / verification_code_service / pricing / service_result
    └── 应用服务：用户侧 user_identity / wallet / charge_flow / station / navigation / order_review
                  管理侧 admin_auth / admin_account / admin_user / admin_station / admin_ops
                  analytics(Dashboard+Ml)
```

依赖方向：应用服务 → 端口接口 + 横切组件 → domain。生产适配器在 `infrastructure/sqlite`；
接线在 `server/main.cpp`（如调价查询 lambda 在 `server/main.cpp:230`）。

---

## 1. 公共错误契约

### include/ncs/core/error.h（Qt 客户端侧）
- `enum class ErrorCode : int`：取值 0~23、401、403，与 domain 版逐值相同。
- `struct AppError`：`code` + `diagnostic`（诊断）、`userMessage`（展示）、`requestId`（排查），全 QString。
- `errorCodeName()` 返回稳定字符串码（实现在 src/error.cpp，switch 全覆盖，兜底 "INTERNAL_ERROR"）。

### include/ncs/core/result.h
- `Result<T>`：variant 二选一，`success()/failure()` 构造，`hasValue()/operator bool` 判定。
- `Result<void>` 特化：布尔 + error_ 成员；失败时调 `value()` 会抛 `std::get` 异常，必须先判定。

### core/domain/error_code.h（服务端权威契约）
- 同名枚举 + 两个 constexpr 映射：
  - `httpStatus()`：InvalidArgument→400；ValidationFailed/CodeInvalid/CodeExpired→422；
    DatabaseError/TransactionFailed/ExternalServiceUnavailable→503；NotFound→404；
    AlreadyExists 及全部业务冲突类→409；UserFrozen/Forbidden→403；InternalError→500；
    RateLimited→429；ReauthRequired/Unauthorized→401；默认 500。
  - `errorCodeName()`：string_view 版。
- 只增不改；与 docs/database-api.md §1.10 一一对应。

> 审读提示：新增错误码要同步 5 处代码 + 接口文档 + 测试锚点（tests/foundation_tests.cpp:46-49）。
> 两份枚举命名空间不同（`ncs::core` vs `ncs::core::domain`），漏同步不会编译报错，只会静默漂移。

---

## 2. 横切组件

### security_crypto（OpenSSL 原语）
- `secureRandomToken(bytes=32)`：RAND_bytes + base64url（无 padding）。随机源失败抛 runtime_error。
- `sha256Hex()`：EVP_Digest，小写 hex；失败抛异常。
- `PasswordHasher::hash()`：PBKDF2-HMAC-SHA256，16 字节盐 + 32 字节派生，
  格式 `pbkdf2-sha256$<iter>$<salt>$<hash>`；密码长度须 ∈ [minLength,128]（默认 minLength=10），
  迭代 ∈ [10万, 200万]，否则抛 invalid_argument。当前标准 60 万次。
- `verify()`：解析三段 `$`，重算后 `CRYPTO_memcmp` 恒时比较；格式/迭代/长度不合法一律 false。
- `needsRehash()`：迭代 ∈ [10万, 60万) 时 true，供登录成功后透明升级。

### verification_code_service（短信验证码）
- 构造时生成进程内 pepper（`secureRandomToken(32)`）；验证码存储为
  `sha256(pepper|phone|purpose|code)`，不落明文。
- `issue()`：validRequest 校验（手机号 11 位数字、purpose ∈ LOGIN/REGISTER/RESET_PASSWORD）；
  同 phone+purpose 60 秒冷却（返回剩余秒数）；单号每日 20 条（返回距次日 UTC 零点的秒数）；
  每日号码数上限 65536、在用条目上限 10000（CapacityExceeded，附 retryAfter=60）；
  10 分钟有效。仅 `exposeDevelopmentCode` 开启时回显明文验证码（模拟短信）。
- `verify()`：一次性消费；未下发/已消费 → NotFound；错 5 次 → Locked（第 5 次即 Locked）；
  过期 → Expired（同时删除）；成功即删除条目。
- `cleanup()`：清过期/已消费条目与昨日计数。
- 状态全部内存态，重启即失效。

### session_manager（会话与令牌）
- `TokenKind`：User/Administrator/Dashboard/MlTask；`Role`：User/Operator/Owner/Viewer/MlWorker/MlTrainer/MlPredictor。
- `issue()`：principal/deviceId 非空、lifetime>0；User 会话 >30 天、Dashboard >8 小时拒绝。
  同 TokenKind 同设备旧会话被替换（替换后锁外通知吊销）；限额按"同主体同类型非同设备"计数
  （用户 3/管理员 2/大屏 2/ML 1）；**总量容量检查先于替换擦除**（`session_manager.cpp:73-76`，
  保证签发失败不误删要替换的会话）。令牌 = 32 字节随机 → base64url（43 字符），库存 SHA-256 摘要。
- `authenticate()`：令牌长度 43~128；摘要查找；已吊销/过期 → 清除并 nullopt；Dashboard 30 分钟
  无活动强制失效；成功刷新 lastSeenAt 并返回上下文副本。
- `revoke()` 幂等恒 true；`revokeForPrincipal()` 仅当会话确属该主体；`revokePrincipal()` 吊销全部；
  `revokeOtherSessions()` 保留当前会话并重建主体索引。吊销观察者一律锁外回调、异常吞掉。
- `markReauthenticated()` / `hasRecentReauthentication(窗口默认 15 分钟)`：敏感操作再认证支撑。
- `parseBearer()`：严格校验 `Bearer ` 前缀 + base64url 字符集。
- `allowsPath()`：/api/v1/events 全放行；/api/v1/system/health/ready 仅管理员；
  其余按 TokenKind 限定前缀（user/admin/dashboard/internal/ml）。
- > 审读提示：签发失败（如第 4 台设备登录）最终映射为 Unauthorized（401）而非限流码，语义上是
  "会话配额满"，审核接口文档时可确认这一映射是否符合预期。

### event_hub（WebSocket 广播枢纽）
- `EventHubOptions`：maxPeers=100（NFR-P-05）、滑动窗口 256 帧 / 1 MiB / 60 秒、
  ping 30 秒、pong 超时 60 秒、可选 `livenessCheck`（心跳时复查令牌）、`clock` 测试缝。
- `registerPeer()`：同 sessionId 重连即替换（旧连接锁外 close 1001）；容量检查在替换擦除之后，
  因此满载时同会话重连仍可成功；`canAcceptPeer()` 为配检预检。
  userId 仅在 TokenKind::User 且 principal 为 "user:<id>" 时解析。
- `publish()`：全局 sequence++；eventId 按分配时刻生成（occurredAt 保留业务原时间，可以早数天）；
  帧先经 `enqueueLocked`：窗口超限时先丢弃 `charge.progress` 帧，仍超限 → 标记 closing，
  锁外 close 1013（close 可经 onclose 重入本枢纽，故绝不能持锁调）。
- `tickHeartbeat()`：pong 超时 → 锁外 close 4000；到期发 ping（sendText 持锁调用）；
  livenessCheck 在锁外逐个执行，失败/抛异常 → 锁外 close 4002。
- `notifySessionRevoked()`：先移出注册表再锁外"先发 session.revoked 帧、后 close 4001"，
  顺序保证无并发插帧。
- `nextEventIdLocked()`：eventId = `EVyyyymmdd` + 当日 8 位起序号；**时钟回拨不后退日序**
  （`event_hub.cpp:252-263`），防止重发旧日 ID。
- `shutdown()`：只清注册表、置位，不发送任何帧（io 已停时才调）。

### idempotency_service（HTTP 幂等）
- `begin()`：scope 非空 + key 必须是 UUID（36 位含 4 连字符，hex 校验），否则 InvalidKey；
  请求体 SHA-256 作摘要。命中且摘要不同 → Conflict；同摘要无结果 → InProgress；
  有结果 → Replay。新键发放 10 分钟租约令牌（16 字节随机）；容量 65536，超限先 cleanup 再判满。
  过期条目（无结果租约到期 / 有结果非永久到期）即擦除。
- `complete()`：必须租约令牌匹配、未超时、尚无结果；status ≥400 → 删除登记（4xx/5xx 不重放，
  同键重试重新执行）；2xx → 落结果（非永久默认保 7 天）、清租约令牌。
- `executeAndComplete()`：无持久化时直接执行；有持久化时业务写入与结果保存共用
  `persistence->withTransaction`，异常回滚即释放幂等键；落库后以 DB 为准刷新内存缓存。
- `abort()` / `IdempotencyLease`（RAII）：析构自动 abort；`complete()` 成功才解除守卫，
  被 complete 拒绝仍会 abort。abort 内持久化异常吞掉（析构路径不能抛）。
- `checkVersion()`：任一版本 <1 → Invalid；相等 → Match；否则 Conflict（乐观锁比对工具）。
- > 审读提示：`begin()` 在持有 mutex_ 时执行持久化读 IO（`loadFromPersistenceUnlocked`），
  单进程内正确但锁窗口含 DB 延迟；审核吞吐相关 NFR 时可关注。

### bounded_executor（有界线程池）
- 构造：worker 或队列容量为 0 抛 invalid_argument。
- `submit()`：空任务/已停/队满 → false（背压，不阻塞调用线程）。
- `shutdown()`：幂等；worker 排空剩余任务后退出（先判 `stopping_ && tasks_.empty()` 才返回）。
- 任务异常整体吞掉（保证长活线程）。

### pricing（纯函数计价）
- `computePrice()`：电价原样透传；排队调价 = `min(5000, 1000×max(0,等待数))`（每 1 人 +1000bp）；
  与审定调价合并后 clamp 到 [-2000, +4000]；服务费 = 基础×(10000+bp)/10000 四舍五入 →
  保证最终服务费 ∈ 基础的 80%~140%（接口契约 7.13）。总价 = 电价 + 调整后服务费。
- `amountCentForEnergy()`：分 = 毫瓦时 × 分/kWh ÷ 1e6，**半分四舍五入**（+500000 再整除）。
- `energyMwhForDuration()`：瓦·秒 ×10 ÷36（即 ×1000/3600），**向下取整**。
  progress 与 settle 用同一公式，估算与结算一致。
- > 审读提示：电量向下取整、金额四舍五入的舍入方向不对称，是有意为之（对用户略有利/中性），
  变更前应对照 BR-05。

### service_result.h / readiness_probe.h（纯头）
- `ServiceResult<T>{ErrorCode error; optional<T> value; ok()}`：应用服务统一返回约定。
- `ReadinessStatus`（schema/读写/WAL/迁移四项与 `ready()`）；`ReadinessProbe` 端口 +
  `UnavailableReadinessProbe` 兜底实现。

---

## 3. 仓储端口与内存适配器

### charging_repository（充电领域聚合端口 + InMemory 实现）
- 枚举：`ChargerType`（0=AC/1=DC）；`ChargerStatus`（0 空闲/1 占用/2 故障/3 停用/4 重启中）；
  `FlowStatus`（10 排队/20 待报价/30 已预约/40 充电中/50 结算中/60 完成/70 取消/80 结算失败/90 过期/
  **100 待用户确认 PendingConfirmation/110 申诉待审核 AppealPending**）。
- `isActiveFlowStatus()`：10/20/30/40/50/80/100/110 视为活跃。
- `flowEventType(toStatus, reasonCode)`：Outbox 事件类型映射——60→`order.settled`、
  100→`order.ready`、110→`order.appealed`、70 且 reason=APPEAL_APPROVED→`order.cancelled`、
  其余 `flow.updated`。
- 文本辅助函数（flowStatusText 等）返回中文展示文案。
- 端口约束：所有服务变更必须运行在 `withTransaction`（生产映射 SQLite BEGIN IMMEDIATE）。
- `InMemoryChargingRepository`：
  - `seedDemoData()`：3 个北京站点、两区资费（110108 电85/服50，110105 电92/服48）、
    9 根桩，CBD-DC-01 预置故障（便于观察 BR-08 过滤）。
  - `withTransaction()`：持 recursive_mutex，先**全量拷贝快照**，异常时整体还原。
    > 审读提示：快照列表**不含 `outbox_` 与 `nextOutboxId_`**（charging_repository.cpp:164-174），
    > 即事务回滚后 Outbox 事件残留——内存适配器与生产 SQLite 的可观察差异之一。
  - `wallet()` 惰性建零账户；`addFlow()/addOrder()` 重复键抛异常；`activeFlow()` 反向扫描
    （`std::map` 按 flowNo 字典序，反向 ≈ 取最新一条活跃流程，依赖"前缀+日期+序号"的编号单调性）。
  - `addFlowEvent()` 同时写 Outbox：事件类型经 `flowEventType()` 映射（见上）；
    `addChargerStatusEvent()` → `charger.statusChanged`。
  - `pollOutbox()`：deliveryStatus==0 且 availableAt≤now，限 limit 条；
    `markOutboxAttempted()`：次数+1，退避 `min(300, 5×2^min(次数-1,6))` 秒，≥10 次转 dead。

### admin_repository（管理端端口）
- `AdminAccount`（含 `mustChangePassword`、version）；`AdminAccountWriteResult`
  （Success/NotFound/UsernameExists/VersionConflict/HashMismatch）。
- `AuditEvent{actorAdminId, action, targetType, targetId, reason, at}`。
- `PriceAdjustment`（source 限 ML_APPROVED/MANUAL）；`DeviceCommand`（模拟重启）；
  `MlTask`；`BackupRecord`（**storagePath 注释明确禁止序列化出站**）。
- `createBackupSnapshot()/verifyBackupSnapshot()` 在端口上：生产 SQLite 做 Online Backup，
  内存适配器是测试桩。
- cpp 仅 `roleName()` 映射（7 个角色 → 字符串）。

### in_memory_admin_repository + in_memory_admin_accounts（管理端内存实现）
- 构造注入共享的 `ChargingRepository&` 与 `UserAccountRepository&`；站点/桩/资费/用户/流程读写
  全部委托，只自持管理员/审计/调价/命令/ML/备份。
- `seedDemoAdmin()`：admin/123456（Operator+Owner），SRS UC-A-01 开发种子，生产不得依赖。
- `withTransaction()`：先快照自身状态 → 委托 `charging_.withTransaction(work)` → 异常时还原自身。
  与 charging 层形成双层回滚。
- `adminAccounts()`：id 降序分页；`createAdminAccount()`：重名 → UsernameExists；新建固定
  Operator + `mustChangePassword=true` + version=1 + 审计 ADMIN_CREATED。
- `updateAdminAccountStatus()`：version CAS；审计 ADMIN_DISABLED/ENABLED。
- `changeAdminAccountPassword()`：**用旧哈希整体比对代替 version CAS**（HashMismatch），
  成功后清 mustChangePassword；审计 ADMIN_PASSWORD_CHANGED。
  > 审读提示：此方法不做 version 校验，与 updateStatus 的 CAS 策略不同，属有意设计
  > （密码字段由哈希比对充当并发防护），审核时可确认测试覆盖。
- `updateManagedUserStatus()`：admin mutex 内 findById 检查 version → `accounts_.updateStatus`。
  > 审读提示：version 检查与真正写入跨两把锁（admin mutex_ 与 accounts 内部 mutex_），
  > 存在理论竞态窗口；仅影响内存适配器，生产 SQLite 在单事务内完成 CAS。
- `auditEvents()`：最新在前，先过滤后分页（page 越界返回空数组）。
- `createStationWithChargers()`：生成 `CODE-DC-NN` 编码，先查重再逐根插入；中途失败返回 false
  （依赖外层服务事务回滚兜底）。
- `effectivePriceAdjustment()`：取覆盖窗口中 id 最大的一条（即最新审定）。
- `tryFinishMlTask()`：终态 CAS——仅 PENDING/RUNNING 可完成；`allowTimedOut=true` 时放行
  TIMED_OUT（超时收尾与按时完成竞争失败后的补交恢复）。
- `cleanupAdminRecords()`：审计/已完成命令 180 天保留；`pruneBackupsLocked()` 按 NFR-R-03：
  近 7 天每日最新一份 + 近 4 周每周最新一份，失败备份 7 天后清除。
- in_memory_admin_accounts.cpp 单独成文件：让主文件在 clang-format 重排后仍低于
  check.sh 的 700 行上限（NFR-M-01）。

### user_account_repository（用户账号端口 + WalletMirror）
- `UserAccount`：余额/欠款是**非权威镜像**（账本在 ChargingRepository）；
  `hasActiveFlow` 供注销检查。
- `replacePasswordHash()`：对存储摘要做 CAS（防透明重哈希覆盖并发改密），**不递增 version**。
- `WalletMirror`：`applyWalletState()` + `setActiveFlowFlag()`，资料侧镜像同步端口。

### in_memory_user_account_repository
- `updateStatus()` **不校验 expectedVersion**（直接写入并 version+1）——CAS 责任在调用方
  （如 InMemoryAdminRepository 先查后写）。
- `anonymize()`：活跃流程 → ActiveFlowExists；用户名/手机号替换为 `deleted_<id>_<随机>`，
  昵称"已注销用户"，清密码/头像，deleted=true、status=0。

---

## 4. 用户侧应用服务

### user_identity_service（身份）
- 构造即生成 `dummyPasswordHash_`（对固定假密码做一次 60 万次 PBKDF2，启动开销约百毫秒量级）。
- `issueCode()`：**不探测手机号是否注册**（响应不得泄露注册状态）；InvalidRequest→InvalidArgument；
  Cooldown/DailyLimit/CapacityExceeded → RateLimited 且携带 CodeIssueResult（含 retryAfterSec）。
- `registerUser()`：用户名 3~32 位 `[A-Za-z0-9_]` 且**不得纯数字**（避免与手机号在
  findByLoginName 中冲突）；密码 10~128；先查重（AlreadyExists）再验 REGISTER 码；昵称 = 用户+尾 4 位。
- `loginPassword()`：未命中也做 dummy 恒时比较（统一 Unauthorized，防用户名探测）；
  冻结 → UserFrozen；needsRehash → `replacePasswordHash` CAS 尽力重哈希（输给并发改密则保留新哈希）。
- `loginSms()`：未注册自动建号（username=`user_`+sha256(phone) 前 16 位）；建号冲突 → AlreadyExists。
- `updateNickname()`：≤20 个 UTF-8 码点、非空白、无 C0/DEL/C1 控制字符；version CAS。
- `updateCredential()`：凭密码或 RESET_PASSWORD 短信码二选一核身；成功后 revokeOtherSessions。
- `deleteAccount()`：需显式 confirmed + 核身；有活跃流程 → ActiveFlowExists；匿名化后
  revokePrincipal。
- `issueSession()`：principal `user:<id>`、30 天；**会话限额满时 issue 返回 nullopt → 对外 401**。
- `validPhone()`：11 位、'1' 开头。`validDeviceId()`：非空且 ≤128。

### wallet_service（钱包）
- `overview()`：availableCent = balanceCent（不预扣欠款）；金额全部整数分。
- `recharge()`：1~1000000 分（即 1 万元）否则 ValidationFailed；事务内：先抵欠款后入余额；
  RC 充值单 + WT 流水 + 钱包 + 镜像同事务；流水 amountCent 记**正数**（充值），结算流水记负数。
- `transactions()`：typeFilter 必须是合法类型名否则 ValidationFailed；仓储已按最新在前返回，
  本方法只做分页；total 为过滤后全量条数。
- > 审读提示：recharge 本身不做幂等——防重复提交由控制器层的 IdempotencyService 包装（POST 类端点）。

### charge_flow_service（核心状态机，1220 行）
常量：`minimumStartBalanceCent=500`（5 元）；报价 300 秒；预约 900 秒；模拟电池 60,000,000 mWh
（60 kWh）；初始 SOC 20%。`chargeTimeScale_` 由构造注入（生产默认 60 倍）。

- `createFlow()`（事务内）：
  1) 账号检查（删除→NotFound、冻结→UserFrozen）；2) 已有活跃流程 → ActiveFlowExists；
  3) 欠费 → DebtOutstanding；余额 <5 元 → InsufficientBalance；4) 站点不存在/停用/无资费 →
  NotFound/ChargerUnavailable/ChargerUnavailable；
  5) **先循环 `promoteQueueLocked` 晋级存量排队流程**（防止新请求插队），再分配空闲桩；
  6) 指定桩不满足"同站+同型+空闲" → AllocationConflict；未指定则取第一根空闲；
  7) 分到桩：占用 + ALLOCATED 状态事件 + 生成 5 分钟报价 QT（价格含排队/审定调价）→ 状态 20；
     未分到：状态 10 入 FIFO，返回排队位置；8) CREATED 流程事件 + 镜像"进行中"标记。
- `confirmQuote()`（20→30）：再次校验欠款/余额；状态与 quoteNo 不符 → InvalidStateTransition；
  version CAS；报价过期 → QuoteExpired；桩不再 Occupied → ChargerUnavailable；
  建订单 OR（快照单价、功率）+ 流程转 30（reservedUntil=+900，版本+1，清报价）。
- `cancel()`（10/20/30→70）：reasonCode 须 ≤32 位 `[A-Za-z0-9_]`；占桩分支：桩回 Idle +
  状态事件 + 出队 + 清流程桩字段 + 晋级；排队分支：仅出队；活跃订单一并置 70；清镜像标记。
- `start()`（30→40）：targetAmountCent ≥1、balanceFloor ≥0 否则 ValidationFailed；
  预约过期 → ReservationExpired；订单/桩缺失或非占用 → ChargerUnavailable；
  **余额下限 = max(全市最低 5 元, 客户端下限)**（BR-04，客户端只能更严不能更松）；
  订单写入 timeScale 与目标金额。
- `progress()`（只读，仅 40）：模拟秒 = (now-startedAt)×timeScale；电量 = W·s×10/36（取整）；
  金额四舍五入；SOC = min(100, 20 + 电量×80/60e6)。
- `settle()`（40/80→**100 待用户确认**，UC-U-09"先冻结后扣款"）：先落瞬态 50；
  电量/金额按持久化 startedAt 重算后**只冻结账单**——订单落 100（paid=0、debt=0、
  settledAt 清空），流程同步 100（版本+1）；桩回 Idle、totalCount+1、totalMinutes +=
  模拟秒/60；触发晋级；**不发生任何扣款/流水**，镜像"进行中"标记保持 true
  （100/110 属活跃状态，用户不处理就无法再建新流程）；
  **任何异常**：catch 中独立事务落状态 80（**版本号故意不递增**，使同键重试携带旧版本仍能通过
  validFlowVersion），对外仅 TransactionFailed；落 80 本身失败则等下次启动恢复兜底。
- `confirmOrder(userId, orderNo)`（100→60，真正扣款）：仅订单本人；已 60 且有 settledAt 时
  幂等返回小票；要求订单与流程同为 100 且 endedAt 存在，否则 InvalidStateTransition；
  事务内完成全部资金动作——付= min(余额, 应付)、差额转欠款、WT 负金额流水、
  订单/流程落 60 + settledAt、事件 USER_CONFIRMED、镜像余额同步并清"进行中"标记；
  异常整体回滚返回 TransactionFailed。
- `appealOrder(userId, orderNo, reason)`（100→110）：原因 1~500 码点/≤2000 字节/无 NUL/非空白；
  **同原因重复提交幂等返回，不同原因 AlreadyExists**（每单一申诉）；
  订单与流程同步 110、记录 appealReason/appealAt、事件 USER_APPEALED；扣款继续冻结。
- `pendingAppeals()`：扫描状态 110 的流程对应订单，按 appealAt 升序（管理端申诉队列）。
- `approveAppeal(adminId, orderNo)`（110→70 核准申诉）：幂等（已 70 且有 reviewedAt 返回小票）；
  要求订单 110 且未扣款（settledAt 为空）；订单/流程落 70、记 reviewedBy/reviewedAt、
  事件 APPEAL_APPROVED、清镜像标记——**钱从未扣过，无需退款**。
- `orders()`：sort ∈ {""、createdAt、-createdAt}；status 白名单 {60,70,80,90,100,110}；
  仓储默认最新在前，sort==createdAt 才反转为升序。
- `receipt()`：已结束（startedAt+endedAt 齐全）订单可出凭据，否则 NotFound；小票含
  appealReason，100/110 状态下 settledAt 为 0（未扣款）。
- `runMaintenance()`（事务内巡检）：过期报价（20）/过期预约（30）统一置 90、释放桩、
  作废订单（直接置 90，不检查订单当前状态）、清镜像、触发晋级。状态 10 的排队流程**不过期**。
- `recoverAtStartup()`：扫描 10/20/30/40/50/80/**100/110** 恢复镜像"进行中"标记并计数；
  充电中流程按持久化 startedAt 续计费（NFR-R-01），不做状态修复。
- `promoteQueueLocked()`（单步晋级，须持事务）：清理队首失效条目（非 10 状态直接出队）→
  取空闲桩 → 无桩/无资费不动 → 占桩 + PROMOTED 事件 + 出队 + 按**出队后的队列长度**计价 →
  生成新报价，流程 10→20（版本+1）。
- `adminForceRelease()`（20/30→70，BR-11）：reason 文本 2~200 且无控制字符；
  目标桩状态仅 0/2/3/4；桩置指定状态（版本+1）；订单置 70；触发晋级。
- `adminControlledSettle()`（40/80→**100 待用户确认**，实为"受控停止"）：与用户 settle 相同的
  冻结账单逻辑（不扣款）；桩终态仅 0（空闲）或 4（重启中）；**无 version CAS 参数**
  （管理端以服务端状态为准）。
- > 审读提示（嵌套事务）：setStationEnabled/createChargers/createRestartCommand 等会在
  > `repository_.withTransaction` 内再调 `flows_.promoteAvailable()`（其内部又开事务）。
  > 两个适配器都把嵌套调用**并入外层事务**（InMemory 用 recursive_mutex；
  > SQLite 用 thread_local 上下文复用连接，见 `sqlite_repository.cpp:1443-1466`），
  > 所以这是安全的，但审核新增代码时不要破坏这一前提。

### station_service（站点查询）
- `haversineMeter()`：E6 整数坐标 → 米，atan2 形式，+0.5 取整。
- `nearbyStations()`：缺坐标先关键词地理编码，仍失败回退默认北京坐标并置 locationFallback；
  只列启用且有生效资费的站点；指定 chargerType 时该站必须有对应桩；按距离升序分页。
  > 审读提示：每个站点查了两次桩（一次带 type 判存在，一次全量做汇总），数据量大时可优化为一次。
- `stationDetail()`：站点或资费缺失均 NotFound；supportedTypes 由现有桩去重推导。
- `stationChargers()`：status 有效域 0~3（越界 ValidationFailed，注意与管理的 0~4 不同）。
- `stationQuote()`：按当前排队长度 + 审定调价计算完整价格构成（不含 quoteNo，不落库）。

### navigation_service（导航）
- `routeToStation()`：站点 NotFound；起点：坐标 > 关键词地理编码 > 默认北京（locationFallback）；
  `gpsOrigin=true` 时必须带坐标（否则 ValidationFailed）且 `normalizeGps` 成功
  （失败 → ExternalServiceUnavailable）。
- 直线距离 ≤5 米直接跳过外部规划；`usableRoute()` 校验（距离>1、时长>0、折线≥2 点、
  坐标合法、非全同点）；外部路线不可用 → routeFallback：返回直线距离 + 起终点两点折线。
  > 审读提示：**回退路径下 durationSecond 恒为 0**，客户端需兼容（需求侧允许的降级语义）。

### order_review_service（订单评价，UC-U-12）
- `validContent()`：1~500 码点、≤2000 字节、无 NUL、非纯空白（协议边界已做 Unicode trim）。
- `get()`：订单存在且属本人，否则 NotFound；返回 optional（可能未评价）。
- `submit()`：rating 1~5；事务内校验归属 + 已结算（status 60 且有 settledAt）否则
  InvalidStateTransition；**内容与评分完全相同 → 幂等返回已有评价；不同 → AlreadyExists**（每单一评）。
- `stationReviews()`：站不存在 NotFound；评论墙按最新在前限 limit 条；
  作者脱敏：昵称非空用昵称（含"已注销用户"），否则 `前3+****+后4`；phone 明文不出应用层。

---

## 5. 管理侧应用服务

### admin_auth_service（管理员登录）
- `login()/loginDashboard()` → 统一 `loginAs()`：参数越界 → Unauthorized；锁定中 → RateLimited；
  未命中账号用 dummy 哈希恒时比较；**冻结账号计入失败锁定**（`admin_auth_service.cpp:40-46`）；
  连续失败 5 次 → 锁 30 秒（返回 RateLimited）；角色检查：Administrator 需 Operator/Owner，
  Dashboard 需 Operator/Owner/Viewer（Viewer 仅大屏），不符 → Unauthorized（**不计失败**）；
  成功清失败计数，签发 8 小时会话（principal `admin:<id>`，角色随账号）。
- 失败记录表上限 65536；条目在"解锁且 10 分钟无尝试"后清理。
- `reauthenticate()`：校验本人密码 → 会话标记再认证 → 返回 now+15 分钟的 UTC 秒。

### admin_account_service（UC-A-09 管理员账号）
- `create()`：用户名 3~32 `[A-Za-z0-9_]`、密码 10~128、原因 2~200 无控制字符；
  事务内 createAdminAccount（审计同事务）；UsernameExists→AlreadyExists；其余失败→InternalError。
- `updateStatus()`：status ∈{0,1}、expectedVersion ≥1；**OWNER 不得停用当前会话对应的自己**
  （ValidationFailed；另一 OWNER 可操作）；停用成功 → revokePrincipal 吊销该账号全部会话。
- `changeOwnPassword()`：仅本人；先验当前密码（Unauthorized）；事务内以旧哈希 CAS 提交；
  成功 → revokeOtherSessions 保留当前会话。
- > 审读提示：`passwordHasher_.hash(newPassword)`（60 万次 PBKDF2）在 lambda 内求值，
  > 即哈希耗时落在仓储事务窗口内；内存适配器无感，生产 SQLite 下拉长了写锁窗口，量级可接受但值得知晓。

### admin_user_service（用户管理）
- `detail()`：资料 + 活跃会话数 + 活跃流程视图。
- `updateStatus()`：冻结/解冻；version CAS 在仓储实现内；**冻结即吊销该用户全部会话**；
  `activeFlowPreserved = 冻结 && 冻结时存在活跃流程`（在事务提交后查询）。
- `orders()`：用户须存在；查询成功在同一事务内记 `USER_ORDERS_VIEWED` 审计（隐私合规点）。

### admin_station_service（站点/桩/资费/调价/重启命令）
- `createStation()`：编码 2~16 `[A-Za-z0-9_]`、名称≤64、地址≤128、adcode 6 位数字、
  坐标 E6 合法域、初始桩 1~100 根、功率 1~1000000W；**资费必须在事务外预检存在**（否则 ValidationFailed）；
  站点强制 enabled=true、version=1；审计 STATION_CREATED（reason 字段存站点编码）。
- `updateStation()`：patch 语义（仅出现的字段更新）；version CAS；改 adcode 需新区域有生效资费；
  整体校验后版本+1。
- `setStationEnabled()`：停用时站内有活跃流程 → InvalidStateTransition（BR）；启用即触发
  两类桩型队列晋级（并入同一事务，见 charge_flow 的嵌套事务提示）。
- `createChargers()`：≤100 根整体失败；桩编码 1~32、类型/功率校验；批量内+库内查重 → AlreadyExists。
- `setChargerStatus()`：目标仅 0/2/3；**桩上有活跃流程必须走强制释放或重启**（BR-11）→
  InvalidStateTransition；置回 0 触发晋级。
- `createRestartCommand()`（UC-A-05）：Disabled/Restarting 状态拒绝；占用的桩先处置——
  20/30 强制释放、40/80 幂等结算（目标桩状态均为 4 重启中）；处置失败整体回滚（桩不释放）；
  命令 CMD 编号、PENDING、2 秒后到期；审计 RESTART_COMMAND。
- `deviceCommand()`：查询 PENDING 且已到期时顺手触发 `completeDueCommands()` 再读一次（惰性推进）。
- `createTariff()`：价格 0~100000 分；区间与既有版本重叠 → ValidationFailed；
  effectiveTo 未给 → 2100 年远期（4102444800）。
- `createPriceAdjustment()`：幅度 ±2000bp 且为 500 的倍数；source ∈ {ML_APPROVED, MANUAL}；
  站点须存在；effectiveTo > effectiveFrom。
- `completeDueCommands()`（runtime tick）：到期 PENDING 命令——桩处于重启中 → 回 Idle +
  RESTART_SUCCEEDED + 晋级；否则 FAILED（"设备状态已变化"）；桩不存在 → FAILED；
  审计归属按 command.actorId 解析（解析失败则不记）。
- `effectiveAdjustmentBp()`：本文件内未被调用；生产接线在 `server/main.cpp:230`（lambda 直连仓储）。

### admin_ops_service（监管/统计/备份/ML 调度）
- `forceRelease()`：原因校验后委托 ChargeFlowService，成功补 FORCE_RELEASE 审计
  （审计在流程事务之外追加）。
- `revenueStats()`：bucket ∈ day/hour；时间缺省 to=now、from=to-30 天；跨度 ≤90 天；
  桶起点 = settledAt/bucketSec×bucketSec（UTC 对齐）；std::map 保证桶有序输出。
- `chargerStatusStats()`：各状态计数 + operational（空闲+占用）+ healthPercent。
- `createBackup()`：BK 记录 PENDING → 快照（长 IO，**刻意不包事务**）→ 成功 SUCCEEDED /
  失败 FAILED + BACKUP_FAILED 审计 + TransactionFailed。
- `verifyBackup()`：置 RUNNING → 校验 → SUCCEEDED/FAILED + verifiedAt + 审计。
- `startMlTask()`：TRAIN/PREDICT；PREDICT horizon ∈ {1,6,24} 非空；`mlStartMutex_` 串行化；
  先收尾超时任务；同类型运行中直接返回（幂等去重）；先事务登记 RUNNING 再启动子进程；
  启动失败 → CAS 置 FAILED + markPredictionsStale + ExternalServiceUnavailable。
- `mlTask()`：命中"运行中且已超时"先补记 TIMED_OUT 再返回最新状态。
- `completeTimedOutMlTasks()`：训练 600s/预测 120s；CAS 置 TIMED_OUT；launcher_->stop；
  预测标 stale。
- `predictions()`：analytics 端口可空（无实现时返回空数组）。

### analytics_service（驾驶舱 + ML 接入）
- `DashboardService::refresh()`：读事务（`withReadTransaction`，不占 SQLite 写锁）内聚合：
  用户数（非 deleted）、站点数、桩状态分布、30 天逐日营收（**空日补零**）、站点排行
  （电量降序、id 升序）、星期×小时热力图（weekday=(days+3)%7+1，1=周一；小时按 startedAt，
  缺省 settledAt）、24h 预测；`nextDashboardVersion()` 在读事务外取号；快照存内存（mutex 保护）；
  异常 → 旧快照标 stale + TransactionFailed。快照带 schemaVersion/dataVersion/stale 字段。
- `MlService::authorize()`：仅 MlTask 令牌 + MlWorker + principal `ml:<taskNo>`（任务级隔离）；
  RegisterModel 需 MlTrainer，WritePredictions 需 MlPredictor。
- `features()`：任务须运行中；时间窗 ≤90 天、toAt ≤ now+1 天、limit 1~5000、游标合法、
  站点存在；有效上界 = min(toAt, 任务 createdAt) 按小时对齐（不得读任务开始后的数据）；
  **仅游标首页触发一次指标重建**（多页导出看到同一快照）。
- `registerModel()`：仅 TRAIN 运行中任务；algorithm 固定 RandomForestRegressor、schema=1、
  seed=20260901；训练窗合法 ≤90 天；六个指标有限非负；checksum 64 位 hex；
  `qualified = mae<baselineMae && rmse<baselineRmse`；有 artifacts 时先验证暂存工件
  （失败 → ExternalServiceUnavailable），合格才落 artifactPath；无 artifacts（测试）用默认路径。
- `writePredictions()`：仅 PREDICT 运行中任务；1~5000 条；同一 modelVersionNo；
  "BASELINE" 放行，否则模型 qualified 且其训练任务 SUCCEEDED；站点存在、horizon ∈ 任务申报集合、
  generatedAt ∈ [now-1d, now+300s]、targetAt = generatedAt + horizon×3600、
  predictedFreeCount ≤ 该站当前可运行桩数、(station,model,targetAt) 唯一；任一违规整体拒绝；
  通过后 upsert。
- `complete()`：summary ≤500 且无控制字符（tab 除外）；succeeded 与 errorSummary 互斥；
  **工件激活/废弃在事务外**（文件 IO 不拉长写锁），事务内重新校验任务与版本归属；
  `receivedAt ≤ createdAt+timeout` 才可收尾；TIMED_OUT 且截止前补交 → `tryFinishMlTask(allowTimedOut=true)`
  恢复为终态；失败任务 markPredictionsStale。

### CMake（core/CMakeLists.txt 等）
- `ncs_core`（src/error.cpp，PUBLIC Qt6::Core）；`ncs_core_domain`（INTERFACE，零依赖）；
  `ncs_core_application`（AUTOMOC OFF，PUBLIC domain，PRIVATE OpenSSL::Crypto）。

---

## 6. 建议的审核顺序

1. `domain/error_code.h` → 对照 docs/database-api.md §1.10 逐值核对。
2. `charging_repository.h` → 领域结构体与端口全貌（后续一切服务的数据语言）。
3. `charge_flow_service`（先读头文件契约，再读 cpp）→ 状态机正确性、结算双事务、晋级公平性。
4. `session_manager` + `event_hub` → 并发与锁外回调纪律（close 锁外 / sendText 持锁）。
5. `idempotency_service` → 对照控制器包装（server/controller/idempotent_response.cpp）。
6. 管理侧服务 → 乐观锁与审计是否同事务、嵌套事务前提。
7. 内存适配器 → 关注与生产 SQLite 适配器的行为差异点（outbox 回滚残留、updateStatus CAS 位置）。
8. 订单确认/申诉链路（在途改动）→ core 状态 100/110 语义、
   server/controller/order_payment_routes.cpp（用户新路由）、admin 申诉核准入口、
   infrastructure/sqlite v10 迁移与 outbox_dispatcher 的新事件类型——三端口径需一致。
