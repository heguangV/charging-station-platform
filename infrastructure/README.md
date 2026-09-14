# infrastructure：持久化与外部能力适配

`infrastructure` 实现数据库、配置、日志、地图服务和文件系统访问。它将 [`core/application`](../core/application/) 定义的接口落实为具体技术实现，让业务服务不必知道 SQL、外部 HTTP 或文件落盘细节。

## 架构位置

```text
server/main.cpp 装配具体实现
        ├── 应用服务 → 仓储接口 → SqliteRepository → SQLite
        ├── 导航服务 → 地图接口 → TencentGeocoder / TencentRoutePlanner
        └── 分析服务 → 文件接口 → 快照与模型产物

Qt 桌面客户端 → ApplicationConfig / ApplicationLogger
```

只有服务端访问平台业务数据库。客户端即使复用这里的配置和日志代码，也应通过 REST/WebSocket 获取订单、余额和站点数据。

## 目录导航

| 目录 | 主要文件 | 功能 |
| --- | --- | --- |
| [`sqlite/`](sqlite/) | `sqlite_repository.*`、`sqlite_seed.*` | schema 迁移、仓储、事务、Outbox、幂等记录、备份、保留清理和演示数据 |
| [`map/`](map/) | `tencent_geocoder.*`、`tencent_route_planner.*` | 地址解析、GPS 坐标转换、腾讯路线规划及响应解析 |
| [`files/`](files/) | `atomic_snapshot_writer.*` | 临时文件和原子替换方式写入大屏快照，避免读取半成品 |
| [`files/`](files/) | `model_artifact_store.*` | 模型 staging、版本归档、校验、定稿和过期产物清理 |
| [`files/`](files/) | `structured_logger.*` | 服务端结构化日志、级别、模块、请求标识与敏感信息过滤 |
| [`config/`](config/) | `application_config.*` | Qt 应用配置加载、校验和安全摘要 |
| [`logging/`](logging/) | `application_logger.*` | Qt 应用日志初始化和输出 |

注意实际归属：服务端结构化日志在 `files/`；服务端启动配置主要在 [`server/runtime/`](../server/runtime/)；头像持久化属于 SQLite 仓储。不要仅按目录名称判断代码的使用位置。

## SQLite 仓储

[`SqliteRepository`](sqlite/sqlite_repository.h) 同时实现用户、钱包、充电流程、评价、管理员、分析、幂等和业务编号等接口，是服务端持久化的集中入口。

- **连接与事务**：连接归调用线程所有；事务内嵌套仓储调用复用该事务连接。通过 `withTransaction()` 和 `withReadTransaction()` 组织一致性读写，不跨线程共享连接。
- **写入一致性**：参数绑定、写事务、版本检查和唯一索引共同约束写入。钱包、订单、流程及事件等相关变更整体成功或回滚。
- **初始化与升级**：`initialize()` 按 schema 版本执行迁移，`schema_version` 保存版本和校验标识，就绪探针检查数据库可用性。当前代码包含订单评价及确认扣款、申诉字段的迁移。
- **事件可靠性**：业务变化与 Outbox 同事务落库，由服务端 [`OutboxDispatcher`](../server/websocket/outbox_dispatcher.h) 后续投递。
- **演示数据**：`sqlite_seed.cpp` 负责完整演示历史和相关检查；不能用清空业务表的方式处理种子冲突。
- **运维能力**：提供备份记录、备份校验和保留清理等操作，触发逻辑由核心用例及服务端运行时负责。

表结构、索引和迁移规则以[数据库设计](../docs/database-design.md)为准，备份恢复步骤见[运维手册](../docs/operations-guide.md)。

## 地图、配置和文件

地图适配器通过 Qt Network 调用外部服务。部分调用使用局部事件循环和超时等待，应在服务端阻塞工作线程执行，不能直接放进 UI 或 Crow 事件循环。规划失败交由核心层返回明确的降级结果，不能把本地距离估算显示成成功规划的路线。

`ApplicationConfig` 按进程环境变量、指定 `.env` 与默认配置读取 Qt 应用设置；安全摘要用于日志。服务端地图 Key 不应进入客户端配置，实际接入与排障见[腾讯地图说明](../docs/tencent-map-setup.md)。

快照写入保证读者不看到半写文件；模型产物存储维护版本、路径和校验的一致性。训练算法在 [`ml/`](../ml/)，进程生命周期由服务端管理。

## 构建目标

本目录分成多个库，不是一个总库：

| 目标及别名 | 内容 | 主要依赖 |
| --- | --- | --- |
| `ncs_infrastructure` | Qt 应用配置和日志 | `ncs_core`、Qt Core |
| `ncs_infrastructure_sqlite` / `ncs::infrastructure_sqlite` | SQLite 仓储 | 核心应用库、SQLite3、OpenSSL Crypto |
| `ncs_infrastructure_map` / `ncs::infrastructure_map` | 地图适配器 | 核心应用库、Qt Core/Network |
| `ncs_structured_log` / `ncs::structured_log` | 结构化日志、快照和模型文件 | 核心应用库、Qt Core、OpenSSL Crypto |

从**仓库根目录**执行，开发预设要求 CMake 3.24+ 和 Ninja：

```bash
cmake --preset dev
cmake --build --preset dev --target ncs_infrastructure ncs_infrastructure_sqlite ncs_infrastructure_map ncs_structured_log
cmake --build --preset dev --target ncs_sqlite_repository_tests ncs_sqlite_migration_concurrency_tests ncs_sqlite_seed_tests
ctest --test-dir build/dev --output-on-failure -R '^(ncs_sqlite_repository|ncs_sqlite_migration_concurrency|ncs_sqlite_seed)$'
```

地图和日志改动还应检查 `ncs_tencent_route_planner`、`ncs_structured_logger` 等测试。数据库改动重点验证旧库升级、并发、异常回滚和重启读取，并使用临时测试库。

## 扩展建议

新增字段先明确核心模型与接口，再追加兼容迁移，同步读写映射和测试；新增外部服务先实现核心层定义的接口。不在适配器里重复实现订单资格、业务状态规则或 HTTP DTO。更多工程边界见[研发实施指南](../docs/development-guide.md)。
