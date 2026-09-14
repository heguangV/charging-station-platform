# core：业务核心与共享类型

`core` 定义 NCS 充电平台的业务规则、用例服务和仓储接口。客户端请求由服务端调用这里的应用服务完成；本目录不负责绘制界面、注册 HTTP 路由或直接执行 SQLite SQL。

## 架构位置

```text
客户端 → server/controller → core/application → 仓储或外部服务接口
                                                    ↑ 实现并注入
                                             infrastructure
```

应用服务依赖抽象接口，SQLite、地图和模型文件等具体实现由 [`server/main.cpp`](../server/main.cpp) 装配。测试可替换成内存仓储，独立验证业务行为。

## 目录和构建目标

| 位置 | 内容 | CMake 目标 |
| --- | --- | --- |
| [`domain/`](domain/) | 不依赖 Qt 的公开错误码，目前主要是 `error_code.h` | 接口库 `ncs_core_domain`，别名 `ncs::core_domain` |
| [`application/`](application/) | 业务模型、仓储接口、用例服务、会话、幂等和事件机制 | 静态库 `ncs_core_application`，别名 `ncs::core_application` |
| [`include/ncs/core/`](include/ncs/core/) | Qt 侧共享的 `AppError`、`ErrorCode`、`Result<T>` | `ncs_core` 的公开头文件 |
| [`src/error.cpp`](src/error.cpp) | Qt 侧错误码名称转换 | 静态库 `ncs_core` |

当前业务模型并未全部拆到 `domain/`：大部分实体和枚举仍在 `application/charging_repository.h` 等文件中，状态转换由应用服务实现。`ncs_core` 也不是全部业务代码的汇总库：它依赖 Qt Core；`ncs_core_application` 使用 C++17 并链接 OpenSSL Crypto，不依赖 Crow、Qt Widgets 或 SQLite。

## 主要功能与阅读入口

| 功能 | 文件 | 职责 |
| --- | --- | --- |
| 充电主流程 | [`charge_flow_service.h`](application/charge_flow_service.h)、[`charging_repository.h`](application/charging_repository.h) | 请求选桩、排队、报价、预约、开始/结束充电、确认扣款、申诉审核、超时与重启恢复 |
| 钱包和计价 | [`wallet_service.h`](application/wallet_service.h)、[`pricing.h`](application/pricing.h) | 余额、欠费、充值、流水、电量和整数分计价 |
| 用户身份 | [`user_identity_service.h`](application/user_identity_service.h)、[`user_account_repository.h`](application/user_account_repository.h) | 注册登录、资料、凭据、头像、注销和账户访问接口 |
| 会话与安全 | [`session_manager.h`](application/session_manager.h)、[`verification_code_service.h`](application/verification_code_service.h)、[`security_crypto.h`](application/security_crypto.h) | 会话签发/撤销、验证码、摘要和密码相关操作 |
| 电站和导航 | [`station_service.h`](application/station_service.h)、[`navigation_service.h`](application/navigation_service.h) | 站点设备查询、路线规划接口和降级结果 |
| 订单评价 | [`order_review_service.h`](application/order_review_service.h) | 评价资格、每单唯一评价、场站评论墙和作者脱敏 |
| 管理业务 | `admin_auth_service.*`、`admin_account_service.*`、`admin_user_service.*`、`admin_station_service.*`、`admin_ops_service.*` | 管理员认证与账号、用户管理、站点设备、运维、审计和备份用例 |
| 大屏与预测 | [`analytics_service.h`](application/analytics_service.h) | `DashboardService`、`MlService`，分析数据和模型产物接口；训练算法在仓库的 `ml/` |
| 公共执行机制 | [`idempotency_service.h`](application/idempotency_service.h)、[`bounded_executor.h`](application/bounded_executor.h)、[`event_hub.h`](application/event_hub.h)、[`business_numbers.h`](application/business_numbers.h) | 请求幂等、受限后台执行、实时事件分发和业务编号 |

`InMemoryChargingRepository` 和 `in_memory_*` 文件用于演示或测试。正式持久化使用 [`SqliteRepository`](../infrastructure/sqlite/sqlite_repository.h)；内存测试不能替代数据库并发和故障恢复验证。

## 典型业务链路

结束充电时，应用服务读取流程和订单，校验所有权与状态，在仓储事务中冻结电量和金额、释放电桩并生成待确认订单。用户确认后由独立操作扣款和写流水；用户申诉则进入待审核状态，审核通过取消订单。评价仍由 `OrderReviewService` 检查已完成、已结算资格。

HTTP 参数和响应由 `server/controller` 转换，界面展示由客户端处理。完整业务规则见[需求规格](../docs/01-requirements-specification.md)，避免在界面、路由和核心层各维护一份不同规则。

## 开发边界

- 金额使用整数分，电量使用整数毫瓦时，时间使用 UTC Unix 秒。历史订单按保存的价格、功率和倍率快照还原。
- 钱包、订单、流程和事件的相关写入通过仓储事务组织；请求幂等不能替代业务状态检查和数据库唯一约束。
- `ServiceResult<T>` 是应用服务返回类型，Qt 侧 `Result<T>` 是另一套共享类型；访问结果前先判断成功。
- 新增外部能力先定义接口，再在基础设施层实现，由服务端注入；不把外部调用或 UI 依赖引入业务规则。
- 修改公开错误码时同步 `domain/error_code.h`、Qt 错误类型、接口文档和测试；修改仓储接口时同时检查 SQLite 与内存实现。

## 构建和测试

在**仓库根目录**执行；开发预设要求 CMake 3.24+、Ninja 及根工程所需的 Qt 和系统依赖：

```bash
cmake --preset dev
cmake --build --preset dev --target ncs_core ncs_core_application
cmake --build --preset dev --target ncs_charge_flow_service_tests ncs_order_payment_tests ncs_order_review_tests
ctest --test-dir build/dev --output-on-failure -R '^(ncs_charge_flow_service|ncs_order_payment|ncs_order_review)$'
```

CTest 不会自动编译测试程序，需先构建对应目标。会话、幂等、导航等测试也集中在 [`tests/`](../tests/)，按修改范围选择。完整工程约定见[研发实施指南](../docs/development-guide.md)和[根 README](../README.md)。
