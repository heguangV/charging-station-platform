# NCS 数据库设计规范

| 项目 | 内容 |
| --- | --- |
| 文档状态 | 数据库实施口径；接口另见 `database-api.md` |
| 文档版本 | 1.0 |
| 编写日期 | 2026-09-02 |
| 目标 | 冻结数据库语义、表职责、安全策略、性能策略和事务边界 |

## 1. 适用边界

本文只定义数据库端实现规范。当前架构固定为独立 Crow HTTPS REST/WebSocket 服务端独占 SQLite；用户端、管理端、Web 大屏和 ML 模块均不得直接依赖表结构，而应调用 `database-api.md` 定义的接口。

数据库实现采用 SQLite 3 单文件 `charge_platform.db`。SQLite 只允许 Crow 服务端的数据库工作线程访问；客户端和 Web 大屏不得打开数据库文件，ML 通过受限内部接口读取特征并回写预测。

## 2. 已识别差异

### 2.1 必须先冻结的冲突

| 主题 | SRS / 研发指南 | V2.1 附件 | 推荐结论 |
| --- | --- | --- | --- |
| 数据库访问拓扑 | 两个 Qt 程序经公共层访问同一 SQLite | Crow 服务端独占 SQLite，客户端走 REST | 按项目 skill 与 README：Crow 服务端独占 SQLite，其他模块只访问授权 API |
| 时间 | SRS 要求 UTC ISO 8601 `TEXT` | Unix 秒 `INTEGER` | 按项目 skill：统一 UTC Unix 秒 `INTEGER`，界面转换为本地时间 |
| 电量 | SRS/研发指南要求整数毫瓦时 | `REAL` kWh，保留三位 | 使用 `INTEGER` 毫瓦时，DTO 字段名带 `_mwh` |
| 充电流程状态 | 固定 10～90 九态 | 订单 0～3 四态 | `charging_flow.status` 使用 10～90；旧 0～3 状态停止作为新代码契约 |
| 电桩状态 | 至少空闲、使用中、故障、重启中、停用 | 只有空闲、使用中、故障 | 采用五态枚举，并为状态变更保留审计 |
| 价格 | 电费、服务费拆分，支持排队/ML 调整与价格快照 | 站点单一 `price` | 拆分价格；订单保存全部价格快照，不从当前配置反算历史金额 |
| 账号 | 指南还要求用户名/密码、会话、欠费、注销 | 用户表只有手机号、昵称、余额 | 补齐凭据、会话、匿名化和欠费模型；钱包余额不直接放在 `user` 中作为唯一账本 |
| 表范围 | 要求覆盖队列、报价、钱包、欠费、命令、备份、模型版本等 | 只有八张左右核心业务表 | 采用第 4 章分层表集 |
| 删除 | 管理对象以逻辑停用为主 | API 提供物理 `DELETE` | 业务数据默认逻辑停用；只有无引用的错误种子或测试数据允许维护工具物理删除 |
| 幂等 | 分配、取消、结算必须幂等 | 部分创建接口仅“可选”幂等键且缓存 5 分钟 | 所有写命令必须携带持久化幂等键；不能只使用内存或五分钟缓存 |

### 2.2 SRS 内部需要同步修订的遗留描述

- `BR-12` 已固定活动流程为 10～90 九态，但 `UC-U-06`～`UC-U-09` 仍用订单 0～3 判断预约、充电、完成和取消。
- `BR-09` 和数据库章节规定电量为整数毫瓦时、时间为 UTC ISO 8601 文本；附件数据模型仍使用 `REAL kWh` 和 Unix 秒。
- 用户注销要求撤销令牌并匿名化用户名，但旧版 `user` 表没有用户名、登录凭据、会话或注销状态字段。
- 充值流水在旧用例中写作“可选”，但钱包可审计、欠费清偿与幂等结算要求它必须存在。

冻结数据库规范后，应将上述用例中的旧字段和旧状态同步替换，避免测试继续依赖 `status IN (0,1)`。

## 3. 推荐的统一数据规则

### 3.1 类型与命名

| 语义 | SQLite 类型 | 字段命名 | 规则 |
| --- | --- | --- | --- |
| 主键 | `INTEGER` | `id` | `INTEGER PRIMARY KEY`，C++ 使用 `qint64` |
| 金额 | `INTEGER` | `*_cent` | 单位为分，不使用浮点数 |
| 电量 | `INTEGER` | `*_mwh` | 单位为毫瓦时；1 kWh = 1,000,000 mWh |
| 功率 | `INTEGER` | `*_watt` | 单位为瓦；120 kW = 120,000 W |
| 比例 | `INTEGER` | `*_bp` | 基点，100 bp = 1%，避免浮点调价 |
| 时间点 | `INTEGER` | `*_at` | UTC Unix 秒；数据库和接口均不传本地时间字符串 |
| 时长 | `INTEGER` | `*_sec` | 秒 |
| 状态 | `INTEGER` | `status` | 必须有 `CHECK`，代码只使用枚举 |
| 布尔值 | `INTEGER` | `is_*` | `CHECK (value IN (0,1))` |
| 业务编号 | `TEXT` | `*_no` | 唯一且创建后不可修改 |
| JSON 扩展 | `TEXT` | `*_json` | 仅用于低频扩展元数据，不代替核心列 |

表名和字段名统一使用小写蛇形、单数形式。外键统一命名为 `<table>_id`。所有公开 DTO 字段必须带单位后缀，禁止使用含义不明的 `amount`、`energy`、`price`。

### 3.2 固定状态枚举

#### 用户状态 `user.status`

| 值 | 名称 | 含义 |
| ---: | --- | --- |
| 0 | `FROZEN` | 禁止登录和创建新流程 |
| 1 | `ACTIVE` | 正常 |
| 2 | `ANONYMIZED` | 已注销并完成身份匿名化 |

#### 电桩状态 `charger.status`

| 值 | 名称 | 含义 |
| ---: | --- | --- |
| 0 | `IDLE` | 空闲且可分配 |
| 1 | `IN_USE` | 已被活动流程占用 |
| 2 | `FAULT` | 故障，不可分配 |
| 3 | `RESTARTING` | 重启中，不可分配 |
| 4 | `DISABLED` | 逻辑停用，不可分配 |

#### 活动流程状态 `charging_flow.status`

| 值 | 名称 | 是否占用用户活动名额 | 是否占用设备 |
| ---: | --- | --- | --- |
| 10 | `QUEUED` | 是 | 否 |
| 20 | `QUOTE_PENDING` | 是 | 临时分配 |
| 30 | `RESERVED` | 是 | 是 |
| 40 | `CHARGING` | 是 | 是 |
| 50 | `SETTLING` | 是 | 是 |
| 60 | `COMPLETED` | 否 | 否 |
| 70 | `CANCELLED` | 否 | 否 |
| 80 | `SETTLEMENT_FAILED` | 是 | 是，直至人工或自动恢复完成 |
| 90 | `EXPIRED` | 否 | 否 |

`charging_order` 不再定义另一套 0～3 流程状态。订单通过 `flow_id` 关联流程；需要展示订单状态时直接返回流程状态。若确有外部旧代码必须读 0～3，可提供只读兼容视图，不允许新代码写兼容状态。

## 4. 推荐表集

### 4.1 基础与身份表

| 表 | 关键字段 | 职责 |
| --- | --- | --- |
| `schema_version` | `version`, `name`, `checksum`, `applied_at` | 记录不可变迁移 |
| `app_config` | `key`, `value`, `value_type`, `updated_at` | 保存最低起充金额、默认时间倍率等非敏感运行配置 |
| `user` | `id`, `username`, `phone`, `nickname`, `avatar_path`, `status`, `registered_at`, `anonymized_at` | 用户身份与资料，不保存余额真相 |
| `user_credential` | `user_id`, `password_hash`, `hash_algorithm`, `updated_at` | 可选密码登录凭据；摘要优先 Argon2id |
| `admin` | `id`, `username`, `password_hash`, `hash_algorithm`, `status`, `created_at` | 管理员身份和密码摘要 |
| `role` | `id`, `code`, `name` | `OWNER`、`OPERATOR`、`DECISION_VIEWER` 等角色 |
| `admin_role` | `admin_id`, `role_id` | 管理员与角色关联 |
| `auth_session` | `id`, `subject_type`, `subject_id`, `token_hash`, `device_id`, `expires_at`, `revoked_at` | 会话、终端数限制和撤销；只存令牌摘要 |
| `sms_code` | `id`, `phone`, `code_hash`, `expires_at`, `failed_attempts`, `consumed_at`, `created_at` | 模拟验证码；只存摘要，新码使旧码失效 |

关键约束：

- `user.username` 与 `user.phone` 分别唯一；匿名化时替换为不可逆占位值，而不是删除历史业务外键。
- `auth_session.token_hash` 唯一；普通用户最多三个有效终端，管理员最多两个，该规则必须在同一事务内执行。
- 手机号格式、昵称长度等需要数据库 `CHECK` 与服务层校验同时存在。

### 4.2 站点、设备与价格表

| 表 | 关键字段 | 职责 |
| --- | --- | --- |
| `station` | `id`, `code`, `name`, `address`, `adcode`, `latitude_e6`, `longitude_e6`, `business_hours`, `status` | 站点基本信息；经纬度使用百万分之一度整数 |
| `charger` | `id`, `station_id`, `code`, `charge_type`, `power_watt`, `connector_standard`, `status`, `total_count`, `total_duration_sec` | 设备信息和当前可运营状态 |
| `region_tariff` | `id`, `adcode`, `electricity_cent_per_kwh`, `service_cent_per_kwh`, `effective_from`, `effective_to` | 行政区基础价格版本 |
| `price_adjustment` | `id`, `station_id`, `charger_type`, `source`, `adjustment_bp`, `reason`, `approved_by`, `effective_from`, `effective_to` | 排队压力或管理员批准的 ML 服务费调整 |
| `charger_status_history` | `id`, `charger_id`, `from_status`, `to_status`, `reason`, `operator_type`, `operator_id`, `changed_at` | 设备状态审计和故障分析 |

关键约束：

- `station.code`、`charger.code` 唯一；站点存在设备时不得物理删除。
- `station.status`、`charger.status` 使用逻辑停用；所有“可用设备”查询必须排除故障、重启中和停用状态。
- `region_tariff` 同一行政区的有效时间区间不得重叠。

### 4.3 充电、订单、钱包与欠费表

| 表 | 关键字段 | 职责 |
| --- | --- | --- |
| `charging_flow` | `id`, `flow_no`, `user_id`, `station_id`, `charger_type`, `charger_id`, `status`, `queued_at`, `quote_expires_at`, `reserved_until`, `started_at`, `ended_at`, `version` | 排队至结算完成的唯一流程状态机 |
| `charging_order` | `id`, `order_no`, `flow_id`, `user_id`, `station_id`, `charger_id`, `electricity_price_cent_per_kwh`, `service_price_cent_per_kwh`, `queue_adjustment_bp`, `ml_adjustment_bp`, `power_watt`, `time_scale`, `energy_mwh`, `amount_cent`, `started_at`, `ended_at`, `settled_at` | 已确认预约后的业务凭证、计费快照和小票数据 |
| `flow_event` | `id`, `flow_id`, `event_type`, `from_status`, `to_status`, `reason_code`, `actor_type`, `actor_id`, `created_at`, `request_id` | 不可变流程事件与排障证据 |
| `wallet_account` | `user_id`, `balance_cent`, `debt_cent`, `version`, `updated_at` | 钱包当前余额和欠费汇总 |
| `wallet_transaction` | `id`, `transaction_no`, `user_id`, `type`, `amount_cent`, `balance_after_cent`, `debt_after_cent`, `reference_type`, `reference_id`, `idempotency_key`, `created_at` | 不可变钱包账本 |
| `recharge_order` | `id`, `recharge_no`, `user_id`, `requested_cent`, `debt_paid_cent`, `balance_added_cent`, `status`, `idempotency_key`, `created_at`, `completed_at` | 虚拟充值请求及欠费优先清偿结果 |
| `settlement_attempt` | `id`, `order_id`, `attempt_no`, `status`, `amount_cent`, `error_code`, `idempotency_key`, `created_at`, `completed_at` | 结算重试、失败恢复和幂等结果 |
| `idempotency_record` | `scope`, `idempotency_key`, `request_hash`, `result_code`, `result_json`, `expires_at`, `created_at` | 持久化写命令去重和结果重放 |

必要唯一索引：

```sql
CREATE UNIQUE INDEX uq_active_flow_user
ON charging_flow(user_id)
WHERE status IN (10, 20, 30, 40, 50, 80);

CREATE UNIQUE INDEX uq_active_flow_charger
ON charging_flow(charger_id)
WHERE charger_id IS NOT NULL AND status IN (20, 30, 40, 50, 80);

CREATE UNIQUE INDEX uq_wallet_idempotency
ON wallet_transaction(idempotency_key);
```

`charging_flow.version` 和 `wallet_account.version` 用于乐观并发检查；设备分配和结算仍使用 `BEGIN IMMEDIATE`、条件更新及最多三次有界重试。

### 4.4 运维、备份与机器学习表

| 表 | 关键字段 | 职责 |
| --- | --- | --- |
| `device_command` | `id`, `command_no`, `charger_id`, `command_type`, `status`, `reason`, `requested_by`, `requested_at`, `started_at`, `finished_at`, `result_json` | 重启、受控中断、状态设置等命令 |
| `audit_log` | `id`, `actor_type`, `actor_id`, `action`, `target_type`, `target_id`, `request_id`, `detail_json`, `created_at` | 管理和安全审计，保留 180 天 |
| `outbox_event` | `id`, `event_no`, `event_type`, `aggregate_type`, `aggregate_id`, `payload_json`, `status`, `attempt_count`, `next_attempt_at`, `created_at`, `published_at` | 与业务事务同时落库，提交后可靠投递 WebSocket/内部事件 |
| `backup_record` | `id`, `backup_no`, `path`, `checksum`, `size_byte`, `status`, `started_at`, `completed_at`, `restore_verified_at` | 一致性备份及恢复验证记录 |
| `model_version` | `id`, `model_no`, `algorithm`, `feature_schema_version`, `artifact_path`, `metrics_json`, `status`, `trained_at`, `expires_at` | 模型版本、指标与合格状态 |
| `ml_task` | `id`, `task_no`, `task_type`, `status`, `model_version_id`, `requested_at`, `started_at`, `finished_at`, `error_message` | 训练和预测任务互斥及失败记录 |
| `load_prediction` | `id`, `model_version_id`, `station_id`, `generated_at`, `target_at`, `horizon_hour`, `predicted_energy_mwh`, `predicted_free_count`, `is_peak`, `is_stale` | 预测结果 |
| `station_hourly_metric` | `station_id`, `bucket_start_at`, `order_count`, `energy_mwh`, `revenue_cent`, `weather_json` | 连续小时聚合数据，供大屏和 ML 使用 |

## 5. 关系与数据真相

```text
user ──1:1── wallet_account ──1:N── wallet_transaction
 │
 └──1:N── charging_flow ──0:1── charging_order
                         │
                         ├──N:1── station
                         └──N:1── charger

station ──1:N── charger
   │
   ├──N:1── region_tariff
   └──1:N── load_prediction / station_hourly_metric
```

唯一真相规则：

- 当前流程状态以 `charging_flow.status` 为准。
- 历史计费价格以 `charging_order` 快照为准。
- 当前钱包数值以 `wallet_account` 为准，资金变化证据以 `wallet_transaction` 为准。
- 当前设备状态以 `charger.status` 为准，变更历史以 `charger_status_history` 为准。
- `station_hourly_metric` 是可重建聚合，不得反向修改订单。

## 6. 事务边界

下列操作必须各自在一个数据库事务中完成：

1. 创建用户、钱包账户和初始会话。
2. 获取报价设备：条件占用设备、更新流程、保存报价及价格版本。
3. 确认预约：校验报价未过期、写价格快照、创建订单、流程转预约。
4. 取消或过期：流程终态、释放设备、记录事件、触发下一位调度标记。
5. 开始充电：校验账号/欠费/余额/归属，流程与订单转充电并写时间及倍率快照。
6. 结算：冻结最终电量和金额、扣余额/记欠费、写钱包流水、更新订单、流程完成、释放设备、更新设备累计值。
7. 充值：创建充值记录、优先清偿欠费、增加余额、分别写钱包流水。
8. 管理员受控重启：登记命令、按需要结算或生成待处理结算、更新设备状态、写审计。

业务变化与 `outbox_event` 必须在同一事务提交。提交后由独立任务发送 WebSocket/内部通知，失败按有界指数退避重试；通知内容不得包含手机号、余额或完整订单。

## 7. 稳定错误码

| code | 名称 | 典型含义 |
| ---: | --- | --- |
| 0 | `OK` | 成功 |
| 1 | `INVALID_ARGUMENT` | 格式或单位错误 |
| 2 | `VALIDATION_FAILED` | 业务校验失败 |
| 3 | `DATABASE_ERROR` | SQLite 打开或读写失败 |
| 4 | `NOT_FOUND` | 目标不存在 |
| 5 | `ALREADY_EXISTS` | 唯一键冲突 |
| 6 | `USER_FROZEN` | 用户被冻结 |
| 7 | `INSUFFICIENT_BALANCE` | 不满足起充金额 |
| 8 | `CHARGER_UNAVAILABLE` | 设备不可分配 |
| 9 | `ACTIVE_FLOW_EXISTS` | 用户已有活动流程 |
| 10 | `ALLOCATION_CONFLICT` | 并发分配失败 |
| 11 | `TRANSACTION_FAILED` | 跨表事务回滚 |
| 12 | `EXTERNAL_SERVICE_UNAVAILABLE` | 地图或 ML 外部能力失败 |
| 13 | `INTERNAL_ERROR` | 未预期内部错误 |
| 14 | `IDEMPOTENCY_CONFLICT` | 同一幂等键对应不同请求 |
| 15 | `INVALID_STATE_TRANSITION` | 非法状态迁移 |
| 16 | `QUOTE_EXPIRED` | 最终报价已过期 |
| 17 | `RESERVATION_EXPIRED` | 预约已过期 |
| 18 | `DEBT_OUTSTANDING` | 存在欠费，禁止新流程 |
| 19 | `RATE_LIMITED` | 验证码等操作过于频繁 |
| 20 | `CODE_INVALID` | 验证码错误或尝试次数超限 |
| 21 | `CODE_EXPIRED` | 验证码过期 |
| 401 | `UNAUTHORIZED` | 未登录或会话失效 |
| 403 | `FORBIDDEN` | 权限不足 |

公开错误码只增不改；新增错误码需要同时修改本文件、枚举定义和协议映射。SQLite 错误、SQL、文件路径和堆栈不得直接返回给 UI。

## 8. 安全、性能、迁移与保留策略

- 数据目录权限设为 `0700`，数据库、WAL、SHM 和备份文件权限设为 `0600`；数据库路径不得位于网络共享目录。
- 每个连接执行 `foreign_keys=ON`、`journal_mode=WAL`、`synchronous=FULL`、`busy_timeout=5000`、`trusted_schema=OFF`；写事务显式选择 `BEGIN IMMEDIATE`。在完成断电与恢复测试后，非结算类批量导入可在隔离数据库临时使用 `synchronous=NORMAL`。
- 连接只属于服务端专用数据库工作线程，连接池按工作线程一对一建立；不跨线程复用连接，不在 Crow 事件循环执行 SQL。
- 密码使用 Argon2id PHC 字符串；随机 Bearer Token 只返回一次，数据库保存 SHA-256 摘要；六位验证码保存带服务端密钥的 HMAC-SHA-256，防止离线枚举。
- SQL 全部参数化并使用固定列名和固定排序白名单；接口不接受 SQL、表名、列名或原始 `ORDER BY`。
- 业务写命令必须鉴权、授权、校验状态迁移、携带持久化幂等键并写审计；审计记录禁止原地修改。
- 高频读取使用覆盖索引和分页，禁止无条件全表扫描及无上限列表；统计按小时增量聚合到 `station_hourly_metric`，不在每次大屏请求中扫描全部订单。
- 充电进度根据开始时间和快照实时计算，不每秒写库；只在关键状态变化、结算和周期性恢复检查点落库。
- WAL 达到阈值或低峰时执行受控 `wal_checkpoint(PASSIVE)`，备份前执行检查点并使用 SQLite Online Backup API；业务运行时不得直接复制 `.db` 文件。
- 首次初始化必须持有进程间锁。迁移文件按序号只追加不修改，并在 `schema_version` 保存校验和。
- 演示种子与迁移分离，固定随机种子 `20260901`；重复播种不得产生重复记录。
- 关键索引至少覆盖手机号、用户名、站点状态、设备站点/状态、流程用户/设备/状态、订单用户/时间、钱包用户/时间、预测站点/目标时间。
- 应用日志保留 30 天；审计/运维日志保留 180 天；匿名订单、钱包和充电记录保留 3 年；匿名 ML 聚合保留 1 年；模型 30 天；预测与评估历史 90 天。
- 每日备份保留 7 份、每周备份保留 4 份；至少完成一次隔离恢复验证。数据库、日志、备份和真实密钥均不得提交 Git。

## 9. 已冻结实施决策

| 项目 | 决定 |
| --- | --- |
| 访问拓扑 | Crow 服务端独占 SQLite；其他模块只访问 HTTPS REST/WebSocket 或受限内部接口 |
| 时间格式 | UTC Unix 秒 `INTEGER` |
| 流程状态 | `charging_flow` 10～90 为唯一流程状态；新代码不使用订单 0～3 |
| 钱包 | `wallet_account` 当前值 + 不可变 `wallet_transaction` 账本 |
| 价格 | 电费和服务费拆分，订单保存完整价格、功率和倍率快照 |
| 幂等 | 金融与结算业务键永久唯一；通用请求结果保留至少 7 天 |
| 登录 | 同时支持验证码和用户名/手机号加密码；密码使用 Argon2id |
| 删除 | 业务接口只逻辑停用或匿名化；物理删除仅限离线测试库维护 |
| 通知一致性 | 首期建立 `outbox_event`，事务提交后异步投递并重试 |
| 小时聚合 | 服务端每小时增量生成并幂等 upsert；大屏和 ML 使用同一口径 |
| 经纬度 | `latitude_e6/longitude_e6 INTEGER` |
| 旧状态兼容 | 仅提供只读兼容视图，调用方迁移完成后删除 |

## 10. 验收最低条件

- schema 初始化、重复初始化和版本升级均有自动化测试。
- 两个独立连接同时抢占同一设备时，只能有一个成功。
- 同一用户并发创建流程时，只能保留一个活动流程。
- 结算在任意失败点回滚后，不出现订单完成但未扣款、已扣款但设备未释放等半完成状态。
- 相同幂等键重复充值或结算只产生一份账本结果；不同请求复用同一键返回 `IDEMPOTENCY_CONFLICT`。
- 断开 UI/Socket 后，依据数据库仍能重建活动流程和充电进度。
- 价格调整后，历史订单小票保持原快照不变。
- 数据库损坏、版本过高、锁超时和备份恢复失败都有可识别错误，不崩溃、不静默丢数据。
- 所有模块只能通过 `database-api.md` 访问数据，UI、大屏和 ML 脚本中不存在业务表 SQL。
