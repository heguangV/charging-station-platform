# NCS 数据库设计

| 项目 | 内容 |
| --- | --- |
| 用途 | 定义 SQLite 物理模型、约束、事务和迁移方式 |
| 需求来源 | SRS 的 `BR-*`、`UC-D-*` 及相关用例 |
| 接口契约 | [REST / WebSocket 接口](database-api.md) |
| 版本 | 1.3 |

本文不重复业务流程、错误码、性能指标或数据保留期限。业务语义以 [SRS](01-requirements-specification.md) 为准，公开字段与错误响应以接口文档为准。

## 1. 访问边界

- 数据库为 SQLite 3 单文件 `charge_platform.db`。
- 当前服务架构中仅 `ncs_server` 的数据访问层打开数据库；客户端、大屏和 ML 不依赖表结构。
- 每个数据库连接只属于一个服务端工作线程，不跨线程复用，也不在 Crow 事件循环中执行 SQL。
- 开发、测试和演示使用不同数据库文件。
- 会话与验证码不落库：由 `core/application` 的内存服务管理并设置容量上限，进程重启后全部失效（SRS `UC-D-01` 已同步）。

## 2. 类型与命名

| 语义 | SQLite 类型 | 命名 | 说明 |
| --- | --- | --- | --- |
| 主键 | `INTEGER` | `id` | `INTEGER PRIMARY KEY`，C++ 使用 `qint64` |
| 金额 | `INTEGER` | `*_cent` | 单位为分 |
| 电量 | `INTEGER` | `*_mwh` | 单位为毫瓦时 |
| 功率 | `INTEGER` | `*_watt` | 单位为瓦 |
| 比例 | `INTEGER` | `*_bp` | 单位为基点 |
| 时间点 | `INTEGER` | `*_at` | UTC Unix 秒 |
| 时长 | `INTEGER` | `*_sec` | 单位为秒 |
| 状态 | `INTEGER` | `status` | 使用 `CHECK` 约束 |
| 布尔值 | `INTEGER` | `is_*` | 仅允许 0 或 1 |
| 业务编号 | `TEXT` | `*_no` | 唯一且创建后不可修改 |
| JSON 扩展 | `TEXT` | `*_json` | 不代替需要查询或约束的核心列 |

表名、字段名使用小写蛇形和单数形式；外键命名为 `<table>_id`。身份表使用 `user_account`、`admin_account`，审计日志表使用 `ops_log`（与 SRS `UC-D-01` 一致）。公开 DTO 的命名由接口文档定义。

## 3. 表职责

### 3.1 身份与配置

| 表 | 关键字段 | 职责 |
| --- | --- | --- |
| `schema_version` | `version`, `name`, `checksum`, `applied_at` | 迁移记录 |
| `user_account` | `username`, `phone`, `nickname`, `status`, `deleted`, `version`, 余额/欠费镜像列 | 用户身份与资料；注销置 `deleted` 并匿名化 |
| `user_credential` | `user_id`, `password_hash` | 密码凭据摘要 |
| `user_avatar` | `user_id`, `data`, `content_type`, `etag` | 服务端重新编码后的头像 BLOB 与条件请求标签 |
| `admin_account` | `username`, `password_hash`, `status`, `must_change_password`, `is_demo`, `version` | 管理员身份；`is_demo` 标记演示账号 |
| `admin_role` | `admin_id`, `role` | 管理员角色（`OPERATOR`/`OWNER`/`VIEWER`） |

### 3.2 站点、设备与价格

| 表 | 关键字段 | 职责 |
| --- | --- | --- |
| `station` | `code`, `name`, `address`, `adcode`, `latitude_e6`, `longitude_e6`, `business_hours`, `enabled`, `version` | 站点资料 |
| `charger` | `station_id`, `code`, `charger_type`, `power_watt`, `connector_standard`, `status`, `total_count`, `total_minutes`, `version` | 设备资料、当前状态与累计服务统计 |
| `region_tariff` | `adcode`, `electricity_cent_per_kwh`, `service_cent_per_kwh`, `effective_from`, `effective_to` | 区域基础价格版本，`UNIQUE(adcode, effective_from)` |
| `price_adjustment` | `station_id`, `charger_type`, `source`, `adjustment_bp`, `effective_from`, `effective_to`, `reason` | 服务费调整（`ML_APPROVED`/`MANUAL`） |

### 3.3 充电、订单与钱包

| 表 | 关键字段 | 职责 |
| --- | --- | --- |
| `charging_flow` | `flow_no`, `user_id`, `station_id`, `charger_id`, `charger_type`, `status`, 报价快照, `quote_expires_at`, `reserved_until`, `version` | 唯一活动流程状态机 |
| `flow_event` | `flow_no`, `from_status`, `to_status`, `reason_code`, `at` | 不可变流程事件 |
| `flow_queue` | `station_id`, `charger_type`, `flow_no`, `sequence` | 按"站点+类型"的 FIFO 排队与自动递补 |
| `charging_order` | `order_no`, `flow_no`, 价格/功率/倍率快照, `energy_mwh`, `amount_cent`, `paid_cent`, `debt_added_cent`, 结余字段, `settled_at` | 业务凭证和小票数据 |
| `wallet_account` | `user_id`, `balance_cent`, `debt_cent`, `version` | 钱包当前值 |
| `wallet_transaction` | `transaction_no`, `type`, `amount_cent`, 结余字段, `related_no` | 不可变钱包账本 |
| `recharge_order` | `recharge_no`, 请求/清偿/入账金额, 结余字段, `completed_at` | 虚拟充值 |
| `idempotency_record` | `scope`, `idempotency_key`, `request_digest`, 结果字段, `expires_at`, `lease_*`, `permanent` | 写命令去重、租约与结果重放 |
| `business_sequence` | `prefix`, `utc_day`, `value` | 业务编号按日序列 |

### 3.4 运维、备份与机器学习

| 表 | 职责 |
| --- | --- |
| `device_command` | 设备重启、受控中断和状态命令 |
| `ops_log` | 管理与安全审计（SRS 名称） |
| `outbox_event` | 与业务事务一同提交的可靠事件：`delivery_status` 0 待投递 / 1 已投递 / 2 死信，`delivery_attempts` ≥10 自动转死信；聚合类型含 `charging_flow`（`flow.updated`/`order.settled`）与 `charger`（`charger.statusChanged`）；唯一消费者为 WebSocket 投递器 |
| `backup_record` | 备份元数据与恢复验证结果 |
| `ml_task` | 训练与预测任务状态 |
| `model_version` | 模型产物校验和、特征版本、固定种子、模型/基线评估指标及合格状态 |
| `load_prediction` | 分站点、模型和目标时间唯一的预测结果及陈旧标记 |
| `station_hourly_metric` | 连续补零的可重建小时聚合、快慢充单量及繁忙设备秒数 |
| `dashboard_state` | 大屏快照数据版本的单行状态，供增量快照与 `dashboard.refresh` 判断 |

## 4. 关系与数据真相

```text
user_account ──1:1── user_credential / user_avatar / wallet_account
user_account ──1:N── wallet_transaction
user_account ──1:N── charging_flow ──0:1── charging_order
                                      ├──N:1── station
                                      └──N:1── charger
charging_flow ──1:N── flow_event
flow_queue ──N:1── charging_flow（按站点+类型排队）

station ──1:N── charger
   ├──1:N── region_tariff（按 adcode）
   ├──1:N── price_adjustment
   └──1:N── load_prediction / station_hourly_metric

admin_account ──N:M── admin_role
admin_account ──1:N── ops_log
```

- 流程状态以 `charging_flow.status` 为准，合法值来自 SRS `BR-12`。
- 历史计费以 `charging_order` 快照为准。
- 钱包当前值以 `wallet_account` 为准，变化证据以 `wallet_transaction` 为准；`user_account` 的余额/欠费列为镜像，随同事务写回。
- 设备当前状态以 `charger.status` 为准；管理侧变化证据以 `ops_log` 与 `device_command` 为准，流程驱动的状态变化证据以 `flow_event` 与 `outbox_event` 为准。
- 结算重试与恢复由 `charging_flow.status = 80`、乐观版本与 `idempotency_record` 覆盖，不设独立结算尝试表。
- `station_hourly_metric` 是可重建数据，不得反向修改订单。

## 5. 关键约束与索引

- 用户名、手机号、站点编码、设备编码和所有业务编号分别唯一。
- 状态列必须使用与 SRS 一致的 `CHECK`；应用代码不得写入未定义状态。
- 匿名化使用不可逆占位值，不删除被历史业务引用的用户主键（`user_account.deleted` 标记）。
- 区域价格版本同行政区区间不得重叠（应用校验），`UNIQUE(adcode, effective_from)` 保证起始点唯一。
- 活动流程用部分唯一索引限制同一用户和同一设备的并发占用：

```sql
CREATE UNIQUE INDEX uq_active_flow_user
ON charging_flow(user_id)
WHERE status IN (10, 20, 30, 40, 50, 80);

CREATE UNIQUE INDEX uq_active_flow_charger
ON charging_flow(charger_id)
WHERE charger_id IS NOT NULL
  AND status IN (20, 30, 40, 50, 80);
```

- 查询索引覆盖身份标识、站点/设备状态、流程占用与活动查询、订单时间、钱包时间、审计时间、命令到期、备份与 ML 任务。
- 所有 SQL 参数化；排序字段和列名只能从代码白名单选择。

## 6. 事务边界

以下用例各自在一个事务中完成，详细业务条件直接引用相应 SRS 用例：

| 用例 | 原子写入范围 |
| --- | --- |
| 注册 | `user_account`、`user_credential`、`wallet_account`（会话在内存中签发） |
| 请求流程 | 活动唯一性、入队或设备占用、`flow_event` |
| 确认报价 | 报价校验、价格快照、`charging_order` 和预约状态 |
| 取消或过期 | 流程终态、设备释放、`flow_event`、队列递补 |
| 开始充电 | 前置校验、开始时间、功率和倍率快照 |
| 结算 | `charging_order`、`wallet_account`、`wallet_transaction`、`charging_flow`、`charger` 累计值 |
| 充值 | `recharge_order`、欠费清偿、余额和 `wallet_transaction` |
| 新增电站 | `station` 和请求中的初始 `charger`；任一设备创建失败时整体回滚 |
| 管理操作 | `device_command`、必要结算、设备状态和 `ops_log` |

业务变化与 `outbox_event` 同事务提交；提交后由 WebSocket 投递器异步投递（轮询待投递行 → 发布事件 → 标记已投递）。设备状态变化（`charger.statusChanged`）与对应设备写同事务提交。事务失败整体回滚，不返回部分成功。幂等完成记录与业务写入同事务落库。

## 7. 连接、迁移与备份

- 每个连接启用 `foreign_keys=ON`、`journal_mode=WAL`、`busy_timeout` 和 `trusted_schema=OFF`。
- 写事务按数据竞争风险选择 `BEGIN IMMEDIATE`，并发重试必须有次数和退避上限。
- 初始化持有进程间锁；迁移按序号只追加不修改（当前为 v1 初始用户充电、v2 管理控制面、v3 设备重启态、v4 演示管理员标记、v5 管理查询索引、v6 Dashboard/ML 数据、v7 订单分析索引、v8 `full-demo-seed`），并在 `schema_version` 的 `checksum` 列保存版本名称标签（如 `ncs-v8-full-demo-seed`），迁移执行时与代码内常量比对，不一致即拒绝写入。
- 演示种子与结构迁移同批次幂等执行（`INSERT OR IGNORE`），重复执行不产生重复数据；演示管理员由 v4 提供，SRS `UC-D-02` 的完整演示数据集（5 站点/48 桩/90 天历史）由 v8 灌入，口径见 7.1。注意 v1 迁移体在每次打开数据库时都会以 `INSERT OR IGNORE` 原样重放，因此 v8 已删除的遗留行可能在后续打开时借空出的 id 重新出现；v8 标记存在时每次打开都会重放遗留站清理策略以保持重开幂等（见 7.1）。
- 充电进度由时间与快照计算，只在状态变化、恢复检查点和结算时持久化。
- `outbox_event` 的待投递记录不得清理；已投递记录保留 7 天，死信记录保留 30 天后由维护任务删除，避免事件表无界增长。
- 备份使用 SQLite Online Backup API 或 SRS 允许的等价一致性方式，禁止运行时直接复制数据库文件。
- 数据目录、日志、备份和密钥的权限与保留期限直接遵循 SRS 的 `NFR-S-*`、`NFR-M-04` 和 `NFR-R-03`。

### 7.1 v8：UC-D-02 完整演示种子

v8 在 v7 之后以与既有迁移相同的守卫模式执行一次：`schema_version` 无 `version=8` 行时才在 `BEGIN IMMEDIATE` 事务内灌入种子并写入标记行（`8/full-demo-seed/ncs-v8-full-demo-seed`，标记行的 `applied_at` 即历史锚点），失败整体回滚；标记行已存在时只重放上一节所述的遗留站清理。种子每次运行先自清上一轮 `sim_owner_%` 范围数据（含新四站设备与三区费率、重置既有站累计值），因此丢失标记的库（如测试中物理回滚到 v5 后重开）重灌不会重复。

- **固定网络（幂等对齐，不修改 v1 既有行内容）**：5 个站点——ZGC 中关村 110108（39.977680, 116.316417）、CYGY 朝阳公园 110105（39.933660, 116.480863）、BJN 北京南站 110106（39.858897, 116.410717）、SJS 石景山 110107（39.923461, 116.150611）、TZYH 通州运河 110112（39.910655, 116.679698）。既有站按编码复用，新站与设备自 `MAX(id)+1` 显式分配 id；站点/设备/费率写入全部 `INSERT OR IGNORE`，重复执行不产生重复行。
- **48 台设备**：ZGC 6 直流 + 4 交流（直流含 v1 的 DC-01…03）、CYGY 8+4、BJN 6+4、SJS 4+4、TZYH 4+4，合计 28 直流 + 20 交流；每站直流按编号前 2 台为 60 kW、其余 120 kW，交流统一 7 kW（7000 W）。`charger_type` 1 直流 / 0 交流；`connector_standard` 直流为国标 `GB/T 20234.3`、交流为 `GB/T 20234.2`，v1 遗留裸 `GB/T` 按类型归一化。6 台固定故障桩（ZGC-DC-01、CYGY-DC-01、CYGY-AC-01、BJN-DC-01、SJS-AC-01、TZYH-DC-01）经受保护更新（`status<>2`）置为 `status=2`，其余 42 台为空闲 0。
- **五区电价**：110108 85/50（沿用 v1 行）；110105 仅在仍为 v1 签名 92/48 且 `effective_from=0` 时修复为 90/55（对应 CYGY）；110106 80/45、110107 75/40、110112 70/35 缺失时补插（`effective_from=0`，`effective_to` 同既有行）。
- **v1 遗留演示站（XEQ/CBD）策略**：仅当站点与设备在 `charging_flow`、`charging_order`、`flow_queue`、`price_adjustment`、`station_hourly_metric`、`load_prediction`、`device_command` 共 7 张业务表中均无引用时才删除（先删设备再删站点）；有引用则保留并记偏差——保留站不参与五站口径与订单生成，其 `GB/T` 连接器与费率按上述规则归一化。由于 v1 迁移体每次打开都会重放，被删除设备空出的 id（v1 布局 6-9）会在后续打开被重新填回 `XEQ-*`/`CBD-*` 设备行；v8 标记存在时的每次打开（以及种子运行内部）都会重放清理：按编码前缀删除站点已不存在的遗留设备行。被保留站的设备因编码与 id 均被占用不会重复出现。
- **300 个演示用户与 90 天历史**：`sim_owner_001…300`（手机 `13800001001…13800001300`，注册时间在各自首单前 1-30 天）；固定随机种子（20260901）确定性生成，历史恰好 90 天、结束于锚点当日前一天，每天 80-120 单无缺口；工作日 7-9/17-21 与周末 10-20 为高峰时段；直流/交流 70/30，站点份额 24/28/22/14/12%；每单按“站点+类型”的空闲非故障桩池分配，编号按日从 `+8000` 起步（`FL`/`OR`/`RC`），不触碰线上 `business_sequence`。
- **订单、流水与钱包一致性**：约 9000 单、94% 完成 / 6% 取消，全部到达终态（60/70）；完成单按“功率×时长×60 倍率”结算电量、按“电量×(电+服)”计价（分），单号/流水/价格/时间字段互洽；每单写 4 个（完成）或 3 个（取消）`flow_event`：`10→20 FLOW_CREATED`、`20→30 QUOTE_CONFIRMED`，完成态追加 `30→40 CHARGING_STARTED`、`40→60 USER_STOPPED`，取消态追加 `30→70 USER_CANCELLED`，作为流程驱动的设备状态变化证据；约 900 笔充值（每笔至少 500 分，金额为下笔订单金额的 6-14 倍并取整到元），与 `wallet_transaction`（充值/充电逐笔入账）及订单流水严格配对，注册与每次充电后的余额可单调重放；`outbox_event` 不产生行。
- **小时聚合不直接种子化**：`station_hourly_metric` 由 `dashboard.refresh`（`refreshHourlyMetrics`）依据订单历史重建（90 天 × 24 小时 × 5 站无缺口），避免种子与重建两条口径漂移。

## 8. 数据库验证

- 空目录初始化、重复初始化和顺序升级（如 v5→v7、v1→v8，保留业务数据）；
- UC-D-02 种子口径：五站/48 桩（含 6 故障）、费率修复与连接器归一化、每日 80-120 单与高峰时段、站点份额、钱包与流水可解、重开幂等、XEQ/CBD 删除与保留偏差；
- 外键、`CHECK`、唯一索引和迁移校验和；
- 同一用户或设备的并发分配唯一性；
- 结算、充值和取消的幂等重试与整体回滚；
- 进程重启后的流程、钱包和设备一致性；
- 在线备份、隔离恢复和损坏数据库错误处理。

性能与容量阈值不在本文重复，测试直接使用 SRS 的 `NFR-P-*`。

## 9. 变更记录

| 版本 | 日期 | 变更 |
| --- | --- | --- |
| 1.1 | 2026-09-02 | 初版物理模型 |
| 1.2 | 2026-09-03 | 与实现对齐：身份表更名 `user_account`/`admin_account`，审计表更名 `ops_log`，充值表更名 `recharge_order`；补充 `user_avatar`、`flow_queue`、`business_sequence`；会话/验证码明确为内存态不落库（SRS `UC-D-01` 同步）；移除 `app_config`、`auth_session`、`sms_code`、`charger_status_history`、`settlement_attempt`（设备状态证据由 `ops_log`/`device_command`/`flow_event`/`outbox_event` 承担，结算重试由状态 80 + 版本 + 幂等记录承担）；更新种子范围、迁移版本与验证清单 |
| 1.3 | 2026-09-05 | 与实现对齐：表清单补充 `dashboard_state`；迁移版本更新到 v7 并澄清 `checksum` 列保存的是版本名称标签；验证清单同步顺序升级与并发唯一性的既有测试口径 |
| 1.4 | 2026-09-05 | 迁移版本更新到 v8 `full-demo-seed`：新增 §7.1 UC-D-02 完整演示种子说明（固定网络与设备/费率修复/遗留站 XEQ-CBD 删除与保留偏差及重开清理/生成口径/流程事件映射/小时聚合不直接种子化）；§7 迁移清单与 §8 验证清单同步 v8 口径 |
