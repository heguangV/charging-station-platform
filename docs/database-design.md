# NCS 数据库设计

| 项目 | 内容 |
| --- | --- |
| 用途 | 定义 SQLite 物理模型、约束、事务和迁移方式 |
| 需求来源 | SRS 的 `BR-*`、`UC-D-*` 及相关用例 |
| 接口契约 | [REST / WebSocket 接口](database-api.md) |
| 版本 | 1.2 |

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
- 初始化持有进程间锁；迁移按序号只追加不修改（当前为 v1 初始用户充电、v2 管理控制面、v3 设备重启态、v4 演示管理员标记、v5 管理查询索引、v6 Dashboard/ML 数据），并在 `schema_version` 保存校验和。
- 演示种子与结构迁移同批次幂等执行（`INSERT OR IGNORE`），重复执行不产生重复数据；当前内置 3 个演示站点（含 1 个故障桩）、2 个区域价格版本和 1 个演示管理员。SRS `UC-D-02` 的 5 站点/48 桩/90 天完整历史种子按里程碑另行实施。
- 充电进度由时间与快照计算，只在状态变化、恢复检查点和结算时持久化。
- `outbox_event` 的待投递记录不得清理；已投递记录保留 7 天，死信记录保留 30 天后由维护任务删除，避免事件表无界增长。
- 备份使用 SQLite Online Backup API 或 SRS 允许的等价一致性方式，禁止运行时直接复制数据库文件。
- 数据目录、日志、备份和密钥的权限与保留期限直接遵循 SRS 的 `NFR-S-*`、`NFR-M-04` 和 `NFR-R-03`。

## 8. 数据库验证

- 空目录初始化、重复初始化和顺序升级（v1→v6）；
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
