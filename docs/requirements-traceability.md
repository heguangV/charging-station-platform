# 需求追踪与阶段状态

本表按交付阶段汇总各模块的产物与验证证据，与完整需求矩阵（九列）同步维护。状态定义：完成 = 产物存在且已验证；部分完成 = 已有产物但仍有明确退出条件；未开始 = 尚无实现证据。逐条任务状态以 `docs/01需求矩阵-NCS充电桩管理平台.xls` 的 310 条明细为准，本表不重复其内容。

## 阶段一：工程基础（完成）

| 项目 | 依据 | 产物 | 验证 | 状态 |
| --- | --- | --- | --- | --- |
| 需求与设计基线 | SRS、研发指南 | `docs/` 核心文档 | 冲突复核、`git diff --check` | 完成 |
| 正式 CMake 目标 | 阶段一、NFR-C-01 | `ncs_user`、`ncs_admin`、`ncs_server`、公共库 | 严格警告构建 | 完成 |
| 分层目录与边界 | 研发指南 §2-3 | `apps/`、`server/`、`core/`、`infrastructure/`、`tests/` | CMake 依赖复核 | 完成 |
| 配置基础 | NFR-S-*、NFR-D-* | `ApplicationConfig`、`.env.example` | 有效配置与启动测试 | 完成 |
| 本机开发 HTTP 联调 | NFR-D-01 | 客户端/服务端 `NCS_ALLOW_INSECURE_HTTP`、回环限制、传输日志 | 配置拒绝测试、真实 HTTP 客户端—服务端烟雾测试 | 完成；验收与生产仍强制 HTTPS/WSS |
| 公共错误码 | 接口文档 §2 | `ErrorCode`、`AppError`、`Result` | 错误码单元测试 | 完成 |
| 日志基础 | NFR-M-04 | `ApplicationLogger` | 文件、请求 ID、脱敏测试 | 完成 |
| 测试框架 | 研发指南 §6 | CTest 与单元/数据库/契约/集成/烟雾测试 | 30 项 CTest 注册并运行 | 完成 |
| 持续集成 | 阶段一 | `.github/workflows/ci.yml` | Ubuntu 全链路通过；Windows 检查修复中 | 部分完成 |
| 分支与评审 | 研发指南 §7 | `CODEOWNERS`、PR/Issue 模板 | 已有 Pull Request 按模板评审合并 | 完成 |
| 运行说明 | NFR-D-* | 运维手册、地图接入、发布指南 | 大屏与管理端运行说明待补 | 部分完成 |
| 本机精确基线构建 | NFR-C-01 | Qt 6.2.x + CMake 3.24+ | 本机工具链报告 | 部分完成：Qt 6.2.4 构建通过；本机 CMake 3.22.1 低于正式门槛，CI 使用 3.24+ |

## 阶段二：数据与领域（完成）

| 项目 | 依据 | 产物 | 验证 | 状态 |
| --- | --- | --- | --- | --- |
| 数据库实现 | UC-D-01、UC-D-03 | `infrastructure/sqlite`（28 表、v1-v8 迁移、WAL、`BEGIN IMMEDIATE`、在线备份） | `ncs_sqlite_repository`：初始化、v5→v8 顺序升级保数据、线程级并发唯一性、幂等重放、整体回滚、重启恢复、备份隔离验证、180/90/30/365 天保留清理与损坏库错误路径 | 完成 |
| 领域服务 | BR-01~BR-12 | `core/application`（充电流程、钱包、身份、幂等、价格、管理员服务） | `ncs_charge_flow_service`、`ncs_security_services`、`ncs_idempotency_service` 等逐条断言 BR 约束 | 完成 |
| 完整演示种子 | UC-D-02 | v8 迁移：5 固定站点、48 桩（6 故障）、5 行政区电价、300 用户、90 天约 9000 单/约 900 充值（固定随机种子 `20260901`） | `ncs_sqlite_seed`：五站/设备/电价/历史分布逐条断言、重开幂等、v1→v8 升级与遗留站清理 | 完成 |

## 阶段三：服务端通信（完成）

| 项目 | 依据 | 产物 | 验证 | 状态 |
| --- | --- | --- | --- | --- |
| REST 契约 | 接口文档 §2-§12 | `server/controller` 71 个端点（`/api/v1` 受控注册） | `ncs_api_routes`、`ncs_admin_routes`、`ncs_user_identity_routes`、`ncs_user_business_routes`、`ncs_dashboard_ml_routes`、`ncs_common_http` | 完成 |
| WebSocket 事件 | 接口文档 §13 | `server/websocket`（outbox 投递、进度推送、会话撤销、心跳） | `ncs_websocket_hub`、`ncs_websocket_dispatcher`、`ncs_websocket_routes` | 完成 |
| 服务端端到端 | NFR-D-01 | `tests/server_smoke_test.py` 虚拟客户端 | 真实 TLS 起停、业务全流程、重启恢复 | 完成（服务端侧） |
| 会话与验证码 | UC-D-01 | 内存会话/验证码服务（容量受限、不落库） | `ncs_security_services` | 完成 |

## 阶段四：用户端（部分完成）

| 项目 | 依据 | 产物 | 验证 | 状态 |
| --- | --- | --- | --- | --- |
| 用户端界面 | UC-U-01~UC-U-10 | `apps/user`（验证码/短信登录、登出、导航路线、部分资料走真实 REST；首页、充电、订单等业务数据仍来自演示服务） | `ncs_user_net_tests`、`ncs_user_smoke`；服务端侧由 `ncs_user_business_routes` 覆盖 | 部分完成：缺真实服务端联调与 UI 验收证据 |
| 腾讯地图导航 | UC-U-02、UC-U-04 | 服务端地理编码/路线规划、用户端路线摘要/内嵌地图及最终降级 | 路线应用服务、腾讯响应解析、REST 契约与 UI 烟雾测试；2026-09-05 图形会话验收：登录后驾车（3.3 km/10 分钟）、步行（3.2 km/48 分钟）、公交（4.1 km/30 分钟）均取得腾讯路线并在内嵌腾讯地图绘出折线截图，空 Server Key 时展示本地最终降级、直线距离与浏览器导航入口 | 完成（内嵌地图渲染依赖软件合成，VM 内需 `--disable-gpu-compositing`） |
| 拍照上传头像 | UC-U-11 | 规划 `apps/user/avatar`、拍照对话框、可选 Multimedia 构建与真实 REST 头像闭环 | 规划图片处理、二进制网络、无设备 UI、有/无 Multimedia 构建和实机验收 | 未开始；必须先完成 UC-U-05 客户端真实头像链路 |

## 阶段五：管理端（部分完成）

| 项目 | 依据 | 产物 | 验证 | 状态 |
| --- | --- | --- | --- | --- |
| 管理服务端 | UC-A-01~UC-A-08 | `server/controller` 管理路由（站点/设备/价格/用户/流程/统计/备份/ML）、登录锁定与二次验证 | `ncs_admin_routes` | 完成 |
| 管理服务端（管理员账号） | UC-A-09 | 管理员账号列表、创建（OPERATOR）、启用/停用、本人改密，及首个 OWNER 一次性引导（`--bootstrap-owner` + `NCS_ADMIN_BOOTSTRAP_KEY`） | `ncs_admin_account_routes`、`ncs_sqlite_admin_accounts` | 后端部分完成（管理端界面未实现） |
| 管理端界面 | UC-A-01~UC-A-08 | `apps/admin` 占位骨架 | `ncs_admin_smoke`（仅启动） | 未开始 |

## 阶段六：大屏与机器学习（部分完成）

| 项目 | 依据 | 产物 | 验证 | 状态 |
| --- | --- | --- | --- | --- |
| 大屏服务端 | UC-W-02、UC-W-04 | Dashboard 路由、分析快照、30 秒原子导出 `dashboard.json` | `ncs_dashboard_ml_routes` | 完成 |
| 大屏前端 | UC-W-01~UC-W-04 | `apps/dashboard` 仅样例快照 | — | 未开始 |
| ML 管线 | UC-M-01~UC-M-04 | `ml/`（训练/预测/worker）+ 子进程任务管理 | `ncs_ml_process_manager`、`ncs_periodic_scheduler`、`ncs_dashboard_ml_routes` | 部分完成：任务互斥与超时已验证；30/90 天保留由 `ncs_sqlite_repository` 保留清理块验证；训练/评估质量未验证 |

## 阶段八：新增增强任务（未开始）

| 项目 | 依据 | 规划产物 | 验证 | 状态 |
| --- | --- | --- | --- | --- |
| 拍照头像 | UC-U-11 | `apps/user/avatar`、`AvatarCaptureDialog`、可选 Qt Multimedia 接入 | 专项实施路径 A0～A5 | 未开始 |
| 设备通信模拟器 | UC-X-01 | 规划 `tools/device_link_sim`独立 CMake，通用通信核心、桩端/平台端 GUI | 专项实施路径 S0～S4，5 桩并发验收 | 未开始 |

## 非功能需求状态

| NFR 组 | 状态 | 证据 / 缺口 |
| --- | --- | --- |
| NFR-C-03、NFR-C-04、NFR-M-02、NFR-M-03、NFR-S-02~S-05、NFR-R-01、NFR-R-03 | 完成 | 编译选项与并发基元、分层 grep 无 SQL、458 处参数绑定、手机号脱敏、会话终端数、WS 无敏感数据、启动恢复、备份与 7 天/4 周保留均有测试 |
| NFR-M-01 | 部分完成 | `scripts/check.sh` 行数门禁带存量例外清单，大文件待拆分 |
| NFR-M-04 | 完成 | 结构化日志与脱敏已测；ops_log/device_command 180 天、outbox 7/30 天保留清理已实现并逐边界测试（含外键完整性门禁） |
| NFR-S-01 | 部分完成 | PBKDF2-HMAC-SHA256（600k 次迭代、版本化摘要）代替规格首选 Argon2id，偏差已在安全基线记录 |
| NFR-R-02 | 完成 | 打开失败、锁等待有处理与测试；损坏库三类错误路径（非 SQLite 文件、页 1 数据区破坏、截断）均断言明确报错（`ncs_sqlite_corruption`） |
| NFR-U-01、NFR-U-02、NFR-C-01、NFR-C-02、NFR-D-01 | 部分完成 | 提示/窗口尺寸/跨平台/路径重定位有实现，缺系统性验收；Windows CI 修复中 |
| NFR-P-02、NFR-P-04、NFR-P-05 | 完成 | 营收 30 天聚合微基准（`ncs_sqlite_revenue_bench`：中位 11.8ms、最差 14.3ms）；3000 账号/100 在线/50 排队/48 充电与 20rps 持续/50rps 峰值/100 WS 全量压测证据（`tests/performance/evidence/`，8/8 阈值通过） |
| NFR-P-01、NFR-P-03、NFR-D-02 | 未开始 | 客户端页面刷新 CPU 与严格单机部署未验收 |

## 维护规则

- 状态变化先更新需求矩阵，再同步本表；标"完成"必须同时给出产物与验证证据。
- 当前已知验收欠账：用户端真实联调、管理端 UI、大屏前端、NFR-P-01/P-03 客户端页面性能、NFR-D-02 严格单机部署验收。
