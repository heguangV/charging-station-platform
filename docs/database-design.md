# NCS 数据库设计

| 项目 | 内容 |
| --- | --- |
| 用途 | 定义 SQLite 物理模型、约束、事务和迁移方式 |
| 需求来源 | SRS 的 `BR-*`、`UC-D-*` 及相关用例 |
| 接口契约 | [REST / WebSocket 接口](database-api.md) |
| 版本 | 1.1 |

本文不重复业务流程、错误码、性能指标或数据保留期限。业务语义以 [SRS](01-requirements-specification.md) 为准，公开字段与错误响应以接口文档为准。

## 1. 访问边界

- 数据库为 SQLite 3 单文件 `charge_platform.db`。
- 当前服务架构中仅 `ncs_server` 的数据访问层打开数据库；客户端、大屏和 ML 不依赖表结构。
- 每个数据库连接只属于一个服务端工作线程，不跨线程复用，也不在 Crow 事件循环中执行 SQL。
- 开发、测试和演示使用不同数据库文件。

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

表名、字段名使用小写蛇形和单数形式；外键命名为 `<table>_id`。公开 DTO 的命名由接口文档定义。

## 3. 表职责

### 3.1 身份与配置

| 表 | 关键字段 | 职责 |
| --- | --- | --- |
| `schema_version` | `version`, `name`, `checksum`, `applied_at` | 迁移记录 |
| `app_config` | `key`, `value`, `value_type`, `updated_at` | 非敏感运行配置 |
| `user` | `username`, `phone`, `nickname`, `avatar_path`, `status` | 用户身份与资料 |
| `user_credential` | `user_id`, `password_hash`, `hash_algorithm` | 密码凭据摘要 |
| `admin` | `username`, `password_hash`, `status` | 管理员身份 |
| `role`, `admin_role` | 角色编码及关联键 | 管理员和查看者授权 |
| `auth_session` | `token_hash`, `device_id`, `expires_at`, `revoked_at` | 会话和撤销 |
| `sms_code` | `phone`, `code_hash`, `expires_at`, `failed_attempts` | 模拟验证码摘要 |

### 3.2 站点、设备与价格

| 表 | 关键字段 | 职责 |
| --- | --- | --- |
| `station` | `code`, `name`, `address`, `adcode`, `latitude_e6`, `longitude_e6`, `status` | 站点资料 |
| `charger` | `station_id`, `code`, `charge_type`, `power_watt`, `status` | 设备资料与当前状态 |
| `region_tariff` | `adcode`, `electricity_cent_per_kwh`, `service_cent_per_kwh`, `effective_from`, `effective_to` | 区域基础价格版本 |
| `price_adjustment` | `source`, `adjustment_bp`, `approved_by`, `effective_from`, `effective_to` | 服务费调整 |
| `charger_status_history` | `charger_id`, `from_status`, `to_status`, `reason`, `changed_at` | 状态审计 |

### 3.3 充电、订单与钱包

| 表 | 关键字段 | 职责 |
| --- | --- | --- |
| `charging_flow` | `flow_no`, `user_id`, `station_id`, `charger_id`, `status`, `version` | 唯一活动流程状态机 |
| `charging_order` | `order_no`, `flow_id`, 价格/功率/倍率快照, `energy_mwh`, `amount_cent` | 业务凭证和小票数据 |
| `flow_event` | `flow_id`, `from_status`, `to_status`, `reason_code`, `request_id` | 不可变流程事件 |
| `wallet_account` | `user_id`, `balance_cent`, `debt_cent`, `version` | 钱包当前值 |
| `wallet_transaction` | `transaction_no`, `type`, `amount_cent`, 结余字段, `idempotency_key` | 不可变钱包账本 |
| `recharge_order` | `recharge_no`, 请求/清偿/入账金额, `status`, `idempotency_key` | 虚拟充值 |
| `settlement_attempt` | `order_id`, `attempt_no`, `status`, `error_code`, `idempotency_key` | 结算重试与恢复 |
| `idempotency_record` | `scope`, `idempotency_key`, `request_hash`, 结果字段 | 写命令去重和结果重放 |

### 3.4 运维、备份与机器学习

| 表 | 职责 |
| --- | --- |
| `device_command` | 设备重启、受控中断和状态命令 |
| `audit_log` | 管理与安全审计 |
| `outbox_event` | 与业务事务一同提交的可靠事件 |
| `backup_record` | 备份元数据与恢复验证结果 |
| `model_version` | 模型产物、特征版本和评估指标 |
| `ml_task` | 训练与预测任务状态 |
| `load_prediction` | 分站点预测结果 |
| `station_hourly_metric` | 可重建的小时聚合数据 |

## 4. 关系与数据真相

```text
user ──1:1── wallet_account ──1:N── wallet_transaction
 │
 └──1:N── charging_flow ──0:1── charging_order
                         ├──N:1── station
                         └──N:1── charger

station ──1:N── charger
   ├──N:1── region_tariff
   └──1:N── load_prediction / station_hourly_metric
```

- 流程状态以 `charging_flow.status` 为准，合法值来自 SRS `BR-12`。
- 历史计费以 `charging_order` 快照为准。
- 钱包当前值以 `wallet_account` 为准，变化证据以 `wallet_transaction` 为准。
- 设备当前状态以 `charger.status` 为准，变化证据以 `charger_status_history` 为准。
- `station_hourly_metric` 是可重建数据，不得反向修改订单。

## 5. 关键约束与索引

- 用户名、手机号、站点编码、设备编码和所有业务编号分别唯一。
- 状态列必须使用与 SRS 一致的 `CHECK`；应用代码不得写入未定义状态。
- 匿名化使用不可逆占位值，不删除被历史业务引用的用户主键。
- 区域价格版本的有效时间区间不得重叠。
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

- 查询索引覆盖身份标识、站点/设备状态、流程占用、订单时间、钱包时间和预测目标时间。
- 所有 SQL 参数化；排序字段和列名只能从代码白名单选择。

## 6. 事务边界

以下用例各自在一个事务中完成，详细业务条件直接引用相应 SRS 用例：

| 用例 | 原子写入范围 |
| --- | --- |
| 注册 | 用户、凭据、钱包和初始会话 |
| 请求流程 | 活动唯一性、入队或设备占用、流程事件 |
| 确认报价 | 报价校验、价格快照、订单和预约状态 |
| 取消或过期 | 流程终态、设备释放、事件和调度标记 |
| 开始充电 | 前置校验、开始时间、功率和倍率快照 |
| 结算 | 订单、钱包、欠费、流水、流程、设备和统计 |
| 充值 | 充值单、欠费清偿、余额和钱包流水 |
| 管理操作 | 设备命令、必要结算、设备状态和审计 |

业务变化与 `outbox_event` 同事务提交；提交后异步投递。事务失败整体回滚，不返回部分成功。

## 7. 连接、迁移与备份

- 每个连接启用 `foreign_keys=ON`、`journal_mode=WAL`、`busy_timeout` 和 `trusted_schema=OFF`。
- 写事务按数据竞争风险选择 `BEGIN IMMEDIATE`，并发重试必须有次数和退避上限。
- 初始化持有进程间锁；迁移按序号只追加不修改，并在 `schema_version` 保存校验和。
- 演示种子与迁移分离；重复执行不得产生重复数据。
- 充电进度由时间与快照计算，只在状态变化、恢复检查点和结算时持久化。
- 备份使用 SQLite Online Backup API 或 SRS 允许的等价一致性方式，禁止运行时直接复制数据库文件。
- 数据目录、日志、备份和密钥的权限与保留期限直接遵循 SRS 的 `NFR-S-*`、`NFR-M-04` 和 `NFR-R-03`。

## 8. 数据库验证

- 空目录初始化、重复初始化和顺序升级；
- 外键、`CHECK`、唯一索引和迁移校验和；
- 同一用户或设备的并发分配唯一性；
- 结算、充值和取消的幂等重试与整体回滚；
- 进程重启后的流程、钱包和设备一致性；
- 在线备份、隔离恢复和损坏数据库错误处理。

性能与容量阈值不在本文重复，测试直接使用 SRS 的 `NFR-P-*`。
