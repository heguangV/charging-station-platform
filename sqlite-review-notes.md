# infrastructure/sqlite 审核导读（本地笔记，未跟踪文件，请勿提交）

> 用途：配合人工审核 SQLite 持久化适配器使用。行号基于 2026-09-09 晚间工作区
> （含在途的"订单确认/申诉"改动），代码变更后会漂移，仅作定位参考。
> 物理模型的权威定义在 `docs/database-design.md`；本文档只描述代码实现。

---

## 0. 模块地图

```
infrastructure/sqlite/            目标 ncs_infrastructure_sqlite（静态库，AUTOMOC OFF）
├── sqlite_repository.h/.cpp      唯一持久化适配器：同时实现 core/application 的
│                                 9 个仓储端口（UserAccount/WalletMirror/Charging/
│                                 OrderReview/Readiness/BusinessNumberSequence/
│                                 IdempotencyPersistence/Admin/Analytics），4227 行
├── sqlite_seed.h/.cpp            UC-D-02 演示种子（v8 迁移）+ 遗留站点清理策略
└── CMakeLists.txt                链接 ncs::core_application、OpenSSL::Crypto、SQLite::SQLite3
```

访问纪律：只有 `ncs_server` 经此访问数据库；客户端一律走 REST/WebSocket。
通用约定（文件头注释，`sqlite_repository.cpp:1-7`）：写事务一律 `BEGIN IMMEDIATE`、
失败整体回滚、全部 SQL 参数化、动态排序仅从代码白名单拼接、并发占用靠部分唯一索引兜底、
`version` 列做乐观锁。

---

## 1. 基础设施：连接、语句、事务上下文

### Connection（RAII，`sqlite_repository.cpp:41-86`）
- `sqlite3_open_v2`：READWRITE|CREATE|**FULLMUTEX**；失败抛 `database unavailable`。
- `busy_timeout(5000)`——锁竞争兜底 5 秒。
- 打开即执行 `PRAGMA foreign_keys=ON`、`PRAGMA trusted_schema=OFF`。
- 连接由调用线程现开现关，**不跨线程复用**。

### Statement（RAII，`:90-202`）
- prepare/bind/step 每步校验返回码，非 OK 即抛——杜绝静默失败。
- bind 重载：int64/int/double/string_view/blob/bindNull（均 SQLITE_TRANSIENT）。
- `row()`：ROW→true、DONE→false、其余抛异常；`execute()` 要求 DONE。
- `reset()` 同时 clear_bindings。
- `integer/real/text/isNull/blob` 取列值；text 对 NULL 返回空串。

### 线程本地事务上下文（`:204-224`）
- `TransactionContext{owner, database}` + `thread_local`。
- `useDatabase(owner, path, work)`：若当前线程已在**本仓储**的事务里 → 复用事务连接；
  否则现开一个临时连接执行（自动语句级事务）。这是"嵌套调用并入外层事务"的实现基础。

### withTransaction / withReadTransaction（`:1443-1501`）
- 写事务 `BEGIN IMMEDIATE`（缩小写竞争窗口），读事务普通 `BEGIN`。
- 嵌套调用（`transactionContext.owner == this`）直接执行 `work()`，不再开新事务。
- work 抛异常 → 清上下文 → ROLLBACK（ROLLBACK 失败也吞掉）→ 原样上抛。

> 审读提示：上下文按 owner（仓储实例）判别。同一进程若有多个 SqliteRepository 实例
> 指向同一个库文件，各自事务互不可见复用——当前 server 只建一个实例，前提成立。

---

## 2. 行映射辅助（`:265-606`）

- `userSelect`（`:290-295`）：三表联结 `user_account LEFT JOIN user_credential / user_avatar`，
  固定 15 列；`readUser()` 按下标一一搬运（含 passwordHash/avatar 的 optional 处理）。
- `readStation()/readCharger()`：各 10 列固定序。
- `flowColumns + readFlow()/bindFlow()`：报价 9 个字段**扁平化**为 charging_flow 的列
  （quote_no…quote_expires_at），无 quote 时 bindNull 8~17。
- `orderColumns + readOrder()/bindOrder()`：28 列，含 v10 新增的
  `appeal_reason/appeal_at/reviewed_by/reviewed_at`（订单确认/申诉特性）。
- `bindFlowFilters()/bindManagedUserFilters()`：每个可选条件绑两次，配合 SQL 里的
  `(? IS NULL OR col=?)` 惯用法（参数化前提下实现可选过滤）。
- `readAdminRoles()/readAdmin()`：角色表单独查询（adminAccounts 分页存在 N+1 角色查询，
  管理员量小，可接受）。
- `encodeHorizons()/decodeHorizons()`：ML 任务 horizon 列表 ↔ 逗号串；解析失败返回空表。
- `readMlTask()`：9 列。
- `fileSha256()`（`:609-645`）：64KB 分块读文件的 SHA-256（备份校验用）。
- `restrictOwnerPermissions()`：目录 0700 / 文件 0600（备份目录与备份文件权限收紧，
  Windows 上恒 true）。

---

## 3. 初始化与迁移（`initialize()`，`:678-1130`）

每次打开数据库按序补跑缺失版本；迁移**只追加不修改**，最新版本号与 checksum
常量（`kLatestSchemaVersion=10`，`kLatestSchemaChecksum="ncs-v10-order-confirmation"`，
`:36-37`）必须与库内一致，否则就绪探针判不就绪。

| 版本 | 内容 | 关键点 |
|---|---|---|
| v1 | 建全部核心表 + 遗留演示数据 | **每次打开都原样重放**，靠 `INSERT OR IGNORE` 幂等（`:685-781`）。含 schema_version、business_sequence、idempotency_record、user_account+credential+avatar、wallet_account/transaction、recharge_order、station/charger/region_tariff、charging_flow、flow_event、outbox_event、flow_queue、charging_order + 3 站 9 桩 2 资费。CHECK 约束贯穿（status 枚举、金额≥0、permanent∈{0,1} 等）。 |
| — | 部分唯一索引（v1 体内） | `uq_active_flow_user`：活跃状态流程每用户唯一；`uq_active_flow_charger`：占用中桩每桩唯一——并发下"一用户一活跃流程/一桩一占用"的数据库级兜底。 |
| v2 | 管理控制面 | admin_account+admin_role（角色 CHECK 只允许 OPERATOR/OWNER/VIEWER）、ops_log、price_adjustment（bp 限 ±2000、区间有序）、device_command、ml_task、backup_record + 查询索引。 |
| v3 | 充电桩"重启中"状态 | 重建 charger 表把 status CHECK 扩到 0~4；`foreign_keys=OFF` 期间重建，完成后 `PRAGMA foreign_key_check` 复检，失败整体回滚（`:842-890`）。 |
| v4 | is_demo 标记列 | admin_account 加 `is_demo`（生产停用演示账号的依据）。 |
| v5 | 管理查询索引 | flow 按 created_at、按站/桩的**部分索引**（仅活跃状态行）。 |
| v6 | Dashboard/ML | `ux_ml_task_one_running_type`：每类型同时只允许一个 PENDING/RUNNING 任务（部分唯一索引）；迁移时 SQL 直接清理历史重复运行任务；station_hourly_metric、model_version、load_prediction（PK：站点+模型+目标时间）、dashboard_state 单行版本号。 |
| v7 | 订单分析索引 | (status, settled_at)、(status, COALESCE(started_at, created_at))。 |
| v8 | 演示种子 | 首次打开执行 `applyFullDemoSeed`（见 §5）；**已应用过**则每次打开重放 `removeLegacyStations`（v1 体重放会让被删的遗留设备行复活，需再清一次，`:1026-1048`）。冲突时显式报错回滚，保留 v1~v7 数据。 |
| v9 | 订单评价 | order_review 表（rating 1~5、content 1~500 字符）+ 用户索引。 |
| v10 | 订单确认/申诉 | 重建 charging_flow / charging_order 两表把 status CHECK 扩到 `…,100,110`，并恢复两表上的索引/触发器（`uq_active_flow_user` 的状态清单同步扩）；再加 4 个申诉列。`foreign_keys=OFF` 包裹、写锁内**复查版本号**防并发双跑、`foreign_key_check` 复检（`:1070-1128`）。 |

> 审读提示：
> 1. `:838-840` 的注释——版本检查语句必须立即 reset，否则活跃语句会让后续迁移中的
>    DROP TABLE 拿到 SQLITE_LOCKED。新增迁移时容易踩。
> 2. v10 的表重建是"改 schema 字符串再重建"的通用手法（把 `70,80,90)` 替换为
>    `70,80,90,100,110)`），依赖旧 CHECK 文本精确匹配；再改 CHECK 时这段需同步。
> 3. 迁移在构造函数里执行：`SqliteRepository` 构造 = initialize + refreshReadiness，
>    首次启动含全量种子，耗时主要在 v8。

---

## 4. 各业务域实现（按文件内分段注释定位）

### 用户账户域（`:1132-1437`）
- `findById/findByPhone/findByLoginName`：userSelect + `deleted=0`；登录名匹配 username **或** phone。
- `create()`：单事务写 user_account + user_credential（有密码时）+ wallet_account（version=1）；
  失败后**回查区分** UsernameExists/PhoneExists，无法归因则原样抛（`:1176-1231`）。
- `updateNickname()`：`UPDATE … WHERE id=? AND version=? AND deleted=0`；0 行受影响时回查区分
  VersionConflict/NotFound。后续 updateStatus 不带 version（CAS 由
  `updateManagedUserStatus` 负责，与内存适配器一致）。
- `updateCredential()`：改 username + upsert 凭据，同事务；NotFound 用 `std::out_of_range`
  作为内部信号再转译；异常路径回查判断 UsernameExists。
- `replacePasswordHash()`：**对存储摘要做 CAS**（`WHERE password_hash=?`），不递增 version——
  与透明重哈希的并发约定一致。
- `updateAvatar()`：upsert user_avatar + version+1，同事务；先 findById 预检 NotFound。
- `anonymize()`（UC-U-13）：事务内检查 deleted/has_active_flow（活跃 → ActiveFlowExists）；
  用户名/手机号换成不可逆占位 `deleted_user_<id>` / `deleted_phone_<id>`、昵称"已注销用户"、
  status=0、deleted=1，删除凭据与头像，**主键保留供历史订单外键引用**。
- `applyWalletState()/setActiveFlowFlag()`：user_account 上的两列镜像更新（WalletMirror）。

### 事务入口
见 §1。注意 order_review 的写入（`addOrderReview`）要求调用方已在 ChargingRepository
事务内（与 OrderReviewService 的约定一致）。

### 钱包域（`:1503-1618`）
- `wallet()`：查 wallet_account；**查不到直接抛 "wallet not found"**。
  > 审读提示：内存适配器是"惰性建零账户"，SQLite 适配器是抛异常——行为差异点。
  > 生产路径上 create() 已同步建钱包行，正常流程不会触发；但绕过 create 的数据（如手工导入）会炸。
- `saveWallet()`：UPDATE，0 行受影响抛 "wallet update failed"。
- `addWalletTransaction()`：append 账本（transaction_no UNIQUE）。
- `walletTransactions()`：`(? IS NULL OR type=?)` + 时间窗 + `ORDER BY created_at DESC,id DESC`（最新在前）。
- `addRechargeOrder()`：append 充值单。

### 站点/设备/资费读取（`:1620-1759`）
- `stations()/station()/chargers()/charger()`：参数化过滤，ORDER BY id。
- `saveCharger()`：全列 UPDATE（调用方负责 version）。
- `effectiveTariff()`：adcode + `[effective_from, effective_to]` 命中、`ORDER BY effective_from DESC LIMIT 1`。

### 充电流程域（`:1761-2078`）
- `addFlow()/saveFlow()/flow()/flowsWithStatus()`：共享 flowColumns；flowsWithStatus 按创建时间升序。
- `activeFlow()`：`status IN (10,20,30,40,50,80,100,110)` + `ORDER BY created_at DESC LIMIT 1`
  ——与部分唯一索引、注销拦截同一口径（注释 `:1838-1839`）。
- `addFlowEvent()`：**同一事务**写 flow_event（证据）+ outbox_event（投递）；
  事件类型由 core 的 `flowEventType()` 决定（60→order.settled、100→order.ready、
  110→order.appealed、70+APPEAL_APPROVED→order.cancelled）。
- `addChargerStatusEvent()`：仅写 outbox（charger.statusChanged）。
- `pollOutbox()`：delivery_status=0 且 available_at≤now，按 id 升序 LIMIT。
- `markOutboxDelivered()`：置 1 并记 published_at。
- `markOutboxAttempted()`：**SQL 内原子**完成 attempts+1、指数退避
  `MAX(available_at, now+MIN(300, 5×2^MIN(attempts,6)))`、≥10 次转死信（`:1984-2012`）——
  语义与内存适配器对齐（内存版在 C++ 里算）。
- `enqueue()/dequeue()/queue()`：flow_queue 表，AUTOINCREMENT sequence 保证 FIFO。

### 订单域（`:2080-2259`）
- `addOrder()/saveOrder()/order()/orderByFlow()/orders()`：共享 orderColumns；
  orders 按 `created_at DESC, order_no DESC`（最新在前），可选状态/时间窗。
- `orderReview()/addOrderReview()/stationReviewRows()`：评价表 CRUD；
  评论墙查询联订单（归属场站）+ 联用户（昵称/手机号，供应用层脱敏）。

### 管理端站点/桩/资费（`:2276-2467`、`:2880-2958`）
- `addStation()/addCharger()`：编码查重；**id 显式取 MAX(id)+1**（与种子分配规则一致，不用 AUTOINCREMENT）。
- `saveStation()`：乐观锁 `WHERE id=? AND version=?`（调用方传入已 +1 的 version，即 expected = version-1）。
- `createStationWithChargers()/addChargers()`：批量前查重（含批次内重复）；中途失败**抛异常**
  依赖外层事务整体回滚（"不产生半座电站"）。
- `tariffOverlaps()`：区间相交 SQL 检查（UNIQUE(adcode, effective_from) 只保证起点唯一）。
- `allFlows()/allOrders()`：全域扫描（供 admin 查询页）。

### 就绪探针（`:2469-2510`）
- `check()` 返回缓存；`refreshReadiness()` 重新探测。
- `probeDatabase()`：最新版本号+checksum 匹配、`PRAGMA journal_mode` == wal、
  `BEGIN IMMEDIATE` + 空更新（`SET applied_at=applied_at`）+ ROLLBACK 验证可写；
  任何异常 → 四项全 false。

### 业务编号与幂等（`:2512-2653`）
- `nextBusinessSequence()`：`INSERT … ON CONFLICT DO UPDATE SET value=value+1 RETURNING value`
  ——前缀+UTC 日原子自增，跨进程唯一。
- 幂等记录 CRUD + `cleanupIdempotencyRecords()`：未完成按租约到期、完成非永久按 expires_at 删除；
  **permanent=1 永不清理**（充值/结算等要求永久唯一性）。

### 管理员域（`:2655-2878`、`:4043-4225`）
- `ensureDevelopmentAdmin(enabled)`：false → 停用全部 is_demo 账号；true → admin 不存在才建
  （admin/123456，OPERATOR+OWNER，is_demo=1，must_change_password=0）。
- `bootstrapOwnerAccount()`：`--bootstrap-owner` 一次性引导——已存在**非演示** OWNER 则返回
  nullopt（演示 OWNER 不阻塞）；用户名占用直接抛错；事务内建号+OWNER 角色+审计。
- `adminAccounts()`：id 降序分页。
- `createAdminAccount()`：事务内插入（status=1、must_change_password=1、is_demo=0、version=1）
  + OPERATOR 角色 + ADMIN_CREATED 审计；异常回查转 UsernameExists。
- `updateAdminAccountStatus()`：SQL 级 version CAS + 同事务审计（DISABLED/ENABLED）。
- `changeAdminAccountPassword()`：**旧哈希 CAS**（`WHERE password_hash=?`）+ 清 must_change_password
  + version+1 + 审计——与内存适配器同策略（哈希比对代替版本号）。
- `listManagedUsers()`：排序仅从白名单映射（registeredAt/balanceCent 及其 `-` 降序，防注入）；
  手机号支持精确匹配或后四位 `substr(phone,8)`；LIMIT/OFFSET 分页，先 COUNT。
- `updateManagedUserStatus()`：version CAS + 同事务 USER_FROZEN/UNFROZEN 审计（内存适配器的
  非原子检查点在生产实现里不存在）。
- `addAuditEvent()/auditEvents()`：ops_log 写入/查询；actorId 接受 `admin:<id>` 前缀并剥掉；
  12 个条件占位符全部参数化；`ORDER BY at DESC,id DESC LIMIT/OFFSET`。

### 设备命令域（`:3033-3129`）
- `addDeviceCommand()/deviceCommand()/saveDeviceCommand()/dueDeviceCommands()`：
  到期扫描取 PENDING 且 execute_at≤now 的前 100 条。

### 流程监管（`:3131-3222`）
- `activeFlowOnCharger()`：桩上活跃流程（**不含** 100/110，状态清单 `10,20,30,40,50,80`）。
  > 审读提示：`stationHasActiveFlow()` 的清单**含** 100/110，而 `activeFlowOnCharger()` 不含——
  > 两者口径不同是有意的（100/110 时设备已释放回 Idle，桩上不存在"占用型"活跃流程），
  > 但审核时值得对照 BR-11 的语义确认。
- `stationHasActiveFlow()`：站点级存在性判断（停用站点拦截用）。
- `flows()`：四维可选过滤 + created_at 降序分页（先 COUNT）。
- `settledOrders()`：仅 status=60 且 settled_at 非空、按 settled_at 排序（营收统计输入）。

### ML 任务域（`:3224-3349`）
- `runningMlTask()/overdueMlTasks()/addMlTask()/mlTask()/saveMlTask()`。
- `tryFinishMlTask()`：**SQL 级终态 CAS**——status 仍为 PENDING/RUNNING（allowTimedOut 时含
  TIMED_OUT）才写入，`sqlite3_changes()==1` 即成功。

### 备份域（`:3351-3582`、`pruneBackups :3975-4041`）
- 元数据 CRUD（backup_record）。
- `createBackupSnapshot()`：backupNo 白名单校验 → 落到 `<库文件>.backups/`（权限 0700）→
  **SQLite Online Backup API** 分步复制（每步 256 页，BUSY/LOCKED 重试至多 100×10ms）→
  权限收紧 0600 + 记 SHA-256 与大小；**使用独立读连接**做源（幂等路由可能在写事务内调用，
  sqlite3_backup 不能从持有事务的连接读，`:3485-3489` 注释）；任一步失败删除半成品。
- `verifyBackupSnapshot()`：路径必须在受控备份目录内（weakly_canonical 前缀比对，防篡改
  记录指向任意文件）→ 大小与 SHA-256 一致 → 复制到隔离临时副本跑 `PRAGMA integrity_check`。
- `pruneBackups()`：NFR-R-03——每天最新一份留 7 天 + 每周最新一份留 4 周，失败备份留 1 周；
  **删文件前同样的目录+文件名双重校验**（备份号白名单 + 父目录匹配 + 文件名 == backupNo+".db"）。

### 保留清理（`cleanupAdminRecords :3584-3613`）
- 审计与已完成设备命令 180 天；已投递 Outbox 7 天（按 published_at）；死信 30 天（按 created_at）。

### 小时聚合与预测（`:3615-3973`）
- `refreshHourlyMetrics()`：按天分块 UPSERT（**避免 90 天单事务长期持有写锁导致并发写
  SQLITE_BUSY**，`:3631-3635` 注释）；递归 CTE 生成小时桶，对"已完成订单"按
  `COALESCE(started_at, created_at)` 聚合电量/单数/快慢充/忙秒；空桶补零；toAt 为排他界，
  只物化整点。
- `hourlyMetrics()`：`bucket:station` 复合游标（`bucket_at>? OR (=? AND station_id>?)`），
  **LIMIT limit+1 探测下一页**再截断生成 nextCursor；子查询附该站当前可运营（status 0/1）桩数。
- `nextDashboardVersion()`：单行表 UPSERT RETURNING 原子发号。
- `addModelVersion()/modelVersion()/latestQualifiedModel()`：后者要求 qualified=1 且
  训练任务 SUCCEEDED。
- `upsertPredictions()`：事务内批量 UPSERT（PK 站点+模型+目标时间），逐条 reset 复用语句。
- `predictions()`：`NOT EXISTS` 同站点同目标时间的更新版本过滤（generated_at、model_version_no
  序取最新），避免读侧拿到旧预测。
- `markPredictionsStale()/cleanupAnalytics()`：预测 90 天、小时指标 365 天；
  模型版本 30 天且**保留仍被预测引用的**。

---

## 5. sqlite_seed.cpp（UC-D-02 演示种子）

### 常量与固定网络（`:26-69`）
- 300 个车主（`sim_owner_001..300` / `13800001001..300`）、90 天历史、时间倍率 60、
  **种子序号从 +8000 起**（避开 live business_sequence 的分配区间）、确定性随机流 20260901、
  当日末秒上限 85499（保证结算尾巴不跨 UTC 日）。
- 5 个固定站点（ZGC/CYGY/BJN/SJS/TZYH，各含 120kW/60kW DC 与 AC 数量、订单量占比、电费/服务费）。

### `removeLegacyStations()`（`:250-287`）
- 先删"无主"遗留设备行（XEQ-/CBD- 前缀但对应站点已不存在——v1 重放的复活行）；
  再对 XEQ/CBD 两站检查 7 类业务引用，**零引用才删站**；有引用保留（偏差记录在
  database-design.md）。每次打开数据库（v8 已应用时）都会重放。

### `reconcileFleet()`（`:289-382`）
- UPSERT 5 站 + 48 桩（INSERT OR IGNORE，重跑复用已有行）；**6 个固定故障桩编码**
  强制 status=2；v1 的裸 "GB/T" 连接标准按类型规范化；修复 110105 资费签名（92/48→90/55）；
  补 3 个新区资费；最后按站×类型装配"可用于生成订单的非故障桩池"。

### `validateSeedConflicts()`（`:384-415`）
- 300 个种子身份逐一与 user_account 精确比对；撞名/撞手机号即抛**带恢复指引**的错误
  （"resolve … before upgrading to schema v8"），调用方回滚，数据库保持 v1~v7 状态。

### `applyFullDemoSeed()`（`:417-698`）
执行顺序（调用方已开 BEGIN IMMEDIATE，v8 标记与数据同事务提交）：
1. 冲突预检 → 装配网络与桩池；
2. 生成 90 天、每天 80~120 个充电会话：车主随机不重复抽取、6% 取消、70% 快充、
   按站点占比分派、按工作日/周末峰谷权重选小时；
   每会话模拟 10→20→30→(40→60 | 30→70) 的完整时间线（created/confirmAt/cancelAt/startAt/settleAt）；
   取消单不产电量金额；未取消单电量 = 功率×时长×60 倍率换算、金额按站点单价四舍五入；
3. **钱包重放**（每车主按时间序）：余额不够下一单就先插一笔充值（6~14 倍账单、取整到 100 分），
   逐笔记账生成 wallet_transaction / recharge_order 与最终余额——保证账本与余额自洽；
4. 落库：300 用户（余额=最终值）、每会话 flow+order+4 条状态事件、充电桩 total_count/total_minutes、
   v8 schema_version 标记（checksum 'ncs-v8-full-demo-seed'）。

> 审读提示：
> 1. 种子订单直接落 status=60（旧"结算即完成"模型），与新流程"结算→100 待确认"并存：
>     历史演示数据天然是已确认态，新订单才走 100/110——审核统计类接口（营收、小时聚合）
>     时注意两者都计入 status=60 口径，无冲突。
> 2. 种子用户 `registeredAt` 在首单前 1~29 天随机；昵称"模拟车主 NNN"。
> 3. 种子里的业务编号从 8000 起，与运行期 `BusinessNumbers`（从 1 起）在同一天可能共存
>     而不冲突——这正是 kSequenceOffset 的设计意图。

---

## 6. 建议的审核顺序

1. §1 基础设施（连接/语句/事务上下文）→ 确认线程模型与嵌套语义。
2. `initialize()` 迁移链 → 对照 docs/database-design.md 核对表结构与索引，重点 v3/v10 的
   重建过程与 v8 的冲突策略。
3. 充电流程域 + 订单域 → 与 ChargeFlowService 的状态机逐状态对照（尤其 100/110 新状态、
   部分唯一索引的口径）。
4. 管理员域 → 乐观锁与审计同事务、bootstrap-owner 的 one-shot 语义。
5. 备份域 → Online Backup 的连接选择、路径校验、权限收紧。
6. 幂等与业务编号 → 与 IdempotencyService/BusinessNumbers 的契约对齐。
7. sqlite_seed.cpp → 确定性、幂等重放、冲突中止路径。
