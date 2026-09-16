# 需求追踪与阶段状态

本表按交付阶段汇总各模块的产物与验证证据，与完整需求矩阵（九列）同步维护。状态定义：完成 = 产物存在且已验证；部分完成 = 已有产物但仍有明确退出条件；未开始 = 尚无实现证据。逐条任务状态以 `docs/01需求矩阵-NCS充电桩管理平台.xls` 的 310 条明细为准，本表不重复其内容。

## 阶段一：工程基础（完成）

| 项目 | 依据 | 产物 | 验证 | 状态 |
| --- | --- | --- | --- | --- |
| 需求与设计基线 | SRS、研发指南 | `docs/` 核心文档 | 冲突复核、`git diff --check` | 完成 |
| 正式 CMake 目标 | 阶段一、NFR-C-01 | `ncs_server`、公共库（`ncs_core`、`ncs_infrastructure`、`ncs_agent`）；客户端改为 `apps/user`、`apps/admin` 的 Vue Web，用 npm 构建 | 严格警告构建：C++ 全量构建零错误；两个 Web 前端 `npm run build` 通过 | 完成 |
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

## 阶段二：数据与领域（PostgreSQL 迁移验收中）

| 项目 | 依据 | 产物 | 验证 | 状态 |
| --- | --- | --- | --- | --- |
| 数据库实现 | UC-D-01、UC-D-03 | `infrastructure/postgres`（外部 v1-v9 迁移、identity/RETURNING、行锁/advisory lock、QPSQL 连接上限、pg_dump/pg_restore、SQLite v9 存量迁移）+ `infrastructure/database` 工厂；`ncs_server` 不链接 SQLite | `ncs_postgres_repository_tests` 已在 PostgreSQL 18.6 实例验证初始化、重开幂等、业务流程、8 路钱包并发、小时聚合与逻辑备份；`ncs_sqlite_to_postgres` 验证 v9 全表搬运和行数校验；旧 SQLite 专属测试尚待全部替换为 PG 故障注入/契约测试 | 进行中 |
| 领域服务 | BR-01~BR-12 | `core/application`（充电流程、钱包、身份、幂等、价格、管理员服务） | `ncs_charge_flow_service`、`ncs_security_services`、`ncs_idempotency_service` 等逐条断言 BR 约束 | 完成 |
| 完整演示种子 | UC-D-02 | PostgreSQL v8 迁移：5 固定站点、48 桩（6 故障）、5 行政区电价、300 用户、90 天约 9000 单/约 900 充值（固定随机种子 `20260901`），完成后同步 identity sequence | PostgreSQL 18.6 实测 v1→v9 生成 5 站/48 桩/300 用户/8991 历史订单，在线新增用户 id > 300 且重开不重复；分布逐条断言尚待移植 | 进行中 |

### PostgreSQL 审计修复验证（2026-09-15）

上表 PostgreSQL 18.6 的成功记录来自修复前一轮，不作为本轮回归通过的证据。本轮修复了注销与创建流程竞争、用户/钱包锁序、OWNER 引导的生产数据库 TLS 检查、`.dump` 清理失败保留记录，以及 SQLite 导入的空目标保护、读快照和建表/数据整体回滚；同时拆分数据库配置解析，未增加行数豁免。

- 构建通过：`ncs_server`、`ncs_postgres_repository_tests`、`ncs_sqlite_to_postgres`、`ncs_create_sqlite_v9_fixture`、配置/数据库安全与相关领域单元测试目标。环境为 macOS AppleClang/Homebrew Qt，并非正式 Ubuntu GCC 基线；仍有 SDK 路径与 OpenSSL deployment-target 警告。
- 本轮 CTest 5/5 通过：`ncs_database_config`、`ncs_database_security`、`ncs_security_services`、`ncs_idempotency_service`、`ncs_charge_flow_service`。配置测试不打开 socket；安全单元测试仅验证 TLS 配置策略，实际 CA/对端证书验证仍需真实 PG 测试。
- 当时记录为「尚未通过本轮验收」的用例，已在独立复核中全部实测通过：真实 PG 并发/注销竞争、SQLite 导入失败回滚与非空目标保护、备份实际 `pg_restore`、HTTPS/REST/WebSocket/重启烟雾测试，以及 `ncs_server_config`。当时未通过的原因是受限沙箱拒绝共享内存/端口操作与 Bash 版本，属执行环境限制而非业务代码缺陷。
- `git diff --check`、四个 PG 测试辅助脚本的 `bash -n`、41 个变更 C/C++ 文件的独立 clang-format 与 700 行检查通过（保留原有豁免）。`server_config.cpp` 为 681 行；`scripts/check.sh` 当时因本机 Bash 3 不支持关联数组未能整体执行，后由独立复核在 Bash 5 下完整执行通过（见「独立复核」节）。
- Ubuntu CI 已设 PG 18 必需工具与禁止静默跳过，但尚无本轮 CI 结果。完整 `.xls` 需求矩阵的只读检查已完成（转换临时 `.xlsx` 后读取），但更新写入因审批额度不足被拒而未执行；保留原文件，矩阵同步待完成，相关条目不得标记验收完成。

### 提交后补充修复（2026-09-15，基于 311cf6f）

- PostgreSQL 仓储和种子异常统一脱敏；移除仓储按 SQL 子串猜测命名参数的逻辑。`walEnabled` 保留接口兼容名，改为检查本地崩溃持久化配置，详见接口文档 §12.2。
- PG 与历史 SQLite 的备份清理统一使用维护任务传入时间，修复失败记录随系统日期变化被误清理的问题。PG 集成测试新增七日/四周合并保留、日内去重、稀疏日期、失败记录边界及事务回滚隔离；保留已有删除失败重试与实际归档恢复检查。
- 本轮重新构建 `ncs_server`、`ncs_postgres_repository_tests`、`ncs_postgres_statement_tests`、`ncs_sqlite_repository_tests` 成功。macOS 工具链仍存在前述 SDK/OpenSSL 警告，不作为 Ubuntu 正式基线证据。
- CTest 5/5 通过：`ncs_postgres_statement`、`ncs_database_security`、`ncs_database_config`、`ncs_sqlite_repository`、`ncs_check_script`。语句单元使用内存 SQLite，仅验证公共封装脱敏、字面量位置绑定及纯配置判断，不等同 QPSQL 集成验证。`ncs_check_script` 已重新注册并修正 BSD/GNU sed 兼容性；Bash 5 与 clang-format 在 PATH 时 `./scripts/check.sh` 和 `git diff --check` 通过。
- 本轮执行环境为受限沙箱：运行 `ncs_postgres_repository` 和 `ncs_server_smoke` 时在 `initdb` 被拒绝（共享内存/端口操作），未进入业务断言。该限制属执行环境，不代表业务代码缺陷；同一提交在不受限环境的实测结论见下节。
- 电子表格流程在**读取阶段已成功**将 `.xls` 转换为临时 `.xlsx` 并完成只读检查；阻塞发生在**更新写入阶段** —— 创建更新辅助脚本时被审批系统以工作区额度不足拒绝。因此原 `.xls` 未修改，矩阵同步未执行；本节对应矩阵的错误脱敏、备份保留与故障测试状态仍待同步，相关条目不得标记切库验收完成。

### 独立复核（2026-09-15，同一工作区未提交修复 + 311cf6f）

由独立评审在 macOS 本机对上述修复复跑，环境为 PostgreSQL 18.6、QPSQL/QSQLITE 驱动齐全、Bash 5.3.15、clang-format 23.1.1。

关于共享内存：本机在该权限下 `initdb` 与 `pg_ctl` 启动临时集群均成功，因此能完成下述断言；这不否定上一节在另一权限配置下记录的 `shmget` 拒绝。`dynamic_shared_memory_type=posix` 只决定并行查询的动态共享内存实现，主共享内存段仍在服务端启动时经 `shmget` 分配，故该参数不能作为"不调用 shmget"的依据。两侧结论差异来自执行权限与操作粒度，均属环境事实，不构成业务代码缺陷。

本节结论按本机实测记录：

- 构建：`cmake --preset dev` + `cmake --build --preset dev --parallel 4` 零错误。
- **CTest 43/43 全部通过**（含上一节标为未通过的用例）：`ncs_postgres_repository`、`ncs_sqlite_to_postgres`、`ncs_postgres_statement`、`ncs_server_smoke`、`ncs_sqlite_repository`、`ncs_check_script`、`ncs_server_config`、`ncs_database_config`、`ncs_database_security` 等。`ncs_sqlite_repository` 的备份保留断言经 `pruneBackups(now)` 修复后由失败转为通过。
- `ncs_check_script`：本机原先 PATH 无 clang-format，`find_program` 在 configure 时失败导致该用例**未被注册**；安装 clang-format 23.1.1 并重新 configure 后注册成功并通过。此外该用例此前还实际暴露过 BSD/GNU `sed` 兼容问题（`sed -i` 需带备份后缀），已随本轮修复解决；补齐工具后单独执行 `check_script_test.sh` 输出 `check.sh regression tests passed`。`./scripts/check.sh`（Bash 5）与 `git diff --check` 均通过。
- 前端：`apps/user` 89/89、`apps/admin` 136/136 通过。
- 真实端到端（非测试进程）：服务端连 PostgreSQL 18.6 起停，车主端与管理端浏览器实测取数、AI 助手工具调用、钱包幂等重放、重启后余额与账本持久化均通过。详见本节对应提交的复核记录。
- 仍待独立环境验收（不得标记通过）：生产 CA 的真实 `verify-full` 校验、受限最小权限数据库账户、代表性规模 SQLite→PostgreSQL 迁移与回滚演练、Ubuntu 正式工具链 CI 结果。

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
| 用户端界面（历史 Qt 实现） | UC-U-01~UC-U-10 | 原 `apps/user` Qt Widgets 用户端已由 UC-U-13 的 Vue 3 Web 客户端取代，代码移除 | 原 `ncs_user_net_tests`、`ncs_user_smoke` 随目标一并移除 | 已被 UC-U-13 取代 |
| 统一响应式 Web 用户端 | UC-U-13、NFR-U-02、NFR-U-03 | `apps/user`（Vue 3 + Vite + Vue Router + Pinia）：`src/views`、`src/components`、`src/api`、`src/stores`、`src/services`，PC 侧边栏 / 手机底部导航同一套页面 | `apps/user` 内 `npm run test`（vitest）与 `npm run build`（Vite）；服务端侧由 `ncs_user_business_routes` 覆盖 | 部分完成：前端自动化与构建已通过；394×844 与 1440×900 的真实浏览器验收截图待归档 |
| 腾讯地图导航 | UC-U-02、UC-U-04 | 系统定位优先、WGS84 坐标转换、手动起点、退化路线检查、导航页与明确降级提示 | 2026-09-07：7 项导航/契约/烟雾测试通过；420×760 Qt 页面验证定位失败、模拟起点、路线摘要与空折线状态，见 [验证记录](navigation-fix-2026-09-07.md)。2026-09-05 的真实腾讯地图验收保留为历史证据 | 部分完成：修复与状态回归已验证；VM GeoClue 禁用定位，系统定位成功至真实腾讯底图的整条链路待实机验收 |
| 拍照上传头像 | UC-U-11 | 原 Qt 用户端头像 REST 链路随 UC-U-13 迁移到 Web 车主端；浏览器媒体设备采集仍待实现 | 图片处理、二进制网络、无设备 UI 与浏览器采集验收 | 部分完成：Web 端头像链路已接入 REST，仍缺浏览器摄像头采集 |
| Web 用户端视觉与动效 | NFR-U-04、NFR-U-05 | `apps/user/src/style.css`（设计令牌与骨架）、`src/styles/motion.css`（关键帧/骨架屏/过渡类）、`src/composables/{useReveal,useValueFlash,useReducedMotion}.js`、`src/directives/reveal.js`、`src/components/NavIcon.vue`、`AppSkeleton.vue` | `ncs_user_web` 前端 `npm run test`（`motion` 用例覆盖滚动进入、指令、数据更新高亮、reduced-motion 降级与样式契约）与 `npm run build`；真实浏览器核对桌面/移动两视口的毛玻璃顶栏、卡片层次、SVG 图标与底部导航指示条 | 部分完成：动效与降级已实现并有测试；真实浏览器截图待归档 |
| AI 出行助手 | UC-U-14、BR-13、BR-14、NFR-M-05、NFR-P-06、NFR-S-06、NFR-S-07 | `agent/`（AgentService、AgentTool 抽象、station_search/station_detail/poi_search/route 四个工具）、`infrastructure/ai/`（OpenAI-compatible 客户端与配置）、`infrastructure/map/`（TencentMapClient、POI、路线服务）、`server/controller/agent_controller.*`、`POST /api/v1/user/agent/chat` | `ncs_agent_service`、`ncs_agent_tools`、`ncs_agent_controller`、`ncs_llm_client`（标签 `agent`/`ai`）；前端 `agentChat`/`agentView` 覆盖 loading、error 与结构化渲染 | 部分完成：工具编排、契约与降级路径已验证；真实大模型与真实腾讯 Key 下的端到端验收待配置 |
| 订单评价与场站评论墙 | UC-U-12 | 后端 v9 `order_review` 迁移、`OrderReviewService`、`GET/POST /api/v1/user/orders/{orderNo}/review`（幂等作用域 `u{userId}:review:{orderNo}`）及 `GET /api/v1/user/stations/{id}/reviews` 评论墙（作者脱敏、倒序限量）；桌面端订单卡/小票评价入口、`ReviewDialog` 与场站详情评论板块在线拉取；安卓端订单评价入口与对话框及 `StationDetail` 评论板块（复用同一 REST 契约）；演示模式评论墙含本人评价 | `ncs_order_review_tests`（迁移、所有权、状态、唯一性、幂等、评论墙分组/隔离/排序/脱敏/注销展示/重启持久，标签 contract+integration）；安卓 arm64 debug APK 已重建（含评论墙），真机交互与 UI 验收证据待归档 | 部分完成：后端、桌面端与安卓端实现完成；安卓端待真机验收 |

## 阶段五：管理端（部分完成）

| 项目 | 依据 | 产物 | 验证 | 状态 |
| --- | --- | --- | --- | --- |
| 管理服务端 | UC-A-01~UC-A-08 | `server/controller` 管理路由（站点/设备/价格/用户/流程/统计/备份/ML）、登录锁定与二次验证 | `ncs_admin_routes` | 完成 |
| 管理服务端（管理员账号） | UC-A-09 | 管理员账号列表、创建（OPERATOR）、启用/停用、本人改密，及首个 OWNER 一次性引导（`--bootstrap-owner` + `NCS_ADMIN_BOOTSTRAP_KEY`） | `ncs_admin_account_routes` 与 Web 管理控制台已接入；PostgreSQL 开发管理员已验证，一次性 OWNER 并发测试待补 | 进行中 |
| 管理端界面（原 Qt 实现） | UC-A-01~UC-A-08 | 原 `apps/admin` Qt Widgets 管理端已由 Web 控制台取代，全部源码与 `ncs_admin_ui_contract`、`ncs_admin_smoke`、`ncs_admin_api_smoke` 三个用例一并移除 | 移除后 C++ 全量构建首次零错误通过，管理接口契约仍由 `ncs_admin_routes`、`ncs_admin_account_routes` 覆盖 | 已被 Web 管理端取代 |
| Web 管理控制台 | UC-A-01~UC-A-09、NFR-U-02、NFR-U-04、NFR-U-05 | `apps/admin`（Vue 3 + Vite + ECharts）：登录与重新认证、总览（营收趋势、每日营收与订单、电桩状态与健康度）、站点与基础价格版本、充电桩与远程重启、用户与订单历史、活动流程强制释放、智能预测与 ML 任务、管理员账号、审计日志与备份运维 | `apps/admin` 的 `npm run test`（13 个文件 136 项：信封/错误码/会话、reauth 同键重试、DTO 格式化与畸形数据拒绝、列表 store、表格与 KPI 组件、路由守卫与抽屉、动效降级）与 `npm run build`；服务端侧由 `ncs_admin_routes`、`ncs_admin_account_routes` 覆盖 | 已完成真实浏览器联调：未登录被守卫重定向到 `/login`；`admin` 登录后总览渲染 10 张 KPI + 2 个 ECharts 画布，侧栏 `blur(18px)`、顶栏 `blur(16px)`、14 个内联 SVG 图标；站点/充电桩/用户/活动流程/管理员/运维各页真实取数（10/20/20/20/1 行、0 报错），预测与审计为空态；390×844 下侧栏变为离屏抽屉并可打开。截图见 `screenshots/preview/after/admin-*.png` |

## 阶段六：大屏与机器学习（部分完成）

| 项目 | 依据 | 产物 | 验证 | 状态 |
| --- | --- | --- | --- | --- |
| 大屏服务端 | UC-W-02、UC-W-04 | Dashboard 路由、分析快照、30 秒原子导出 `dashboard.json` | `ncs_dashboard_ml_routes` | 完成 |
| 大屏前端 | UC-W-01~UC-W-04 | `apps/dashboard` Vue 大屏（ECharts 按需图表、登录/会话、受权数据与恢复） | 四分辨率 UI、非空 DTO、XSS 与单位回归（见下方大屏前端接入状态） | 部分完成：设备占比口径与受权快照接口待后端接入 |
| ML 管线 | UC-M-01~UC-M-04 | `ml/`（训练/预测/worker）+ 子进程任务管理 | `ncs_ml_process_manager`、`ncs_periodic_scheduler`、`ncs_dashboard_ml_routes` | 部分完成：任务互斥与超时已验证；PostgreSQL 30/90 天保留契约和训练/评估质量尚待验证 |

### 大屏前端接入状态

本节仅记录 `apps/dashboard/` 前端产物，不变更后端条目状态。详细证据见 [PR7 前端修复记录](dashboard-pr7-fixes.md)。

| 项目 | 依据 | 产物 | 验证 | 状态 |
| --- | --- | --- | --- | --- |
| 大屏布局与图表 | UC-W-01、UC-W-03 | Vue 大屏、ECharts 按需图表、空态和安全 tooltip | 四分辨率 UI、非空 DTO、XSS 和单位回归 | 部分完成：设备数量占比与累计统计口径待后端接入 |
| 受权数据与恢复 | UC-W-02 | 整数单位 DTO、受权 summary、当前会话内存过期提示 | 空数据、错误、恢复、契约测试 | 部分完成：受权文件快照接口及非公开导出目录待后端提供 |
| 登录与会话 | UC-W-04 | 登录/退出、401/403 清空、请求取消、8 小时/30 分钟期限 | 单元/浏览器/CTest | 部分完成：Crow 页面鉴权和即时撤销通知待协作验收 |

## 阶段八：新增增强任务（未开始）

| 项目 | 依据 | 规划产物 | 验证 | 状态 |
| --- | --- | --- | --- | --- |
| 拍照头像 | UC-U-11 | `apps/user/avatar`、`AvatarCaptureDialog`、可选 Qt Multimedia 接入 | 专项实施路径 A0～A5 | 未开始 |
| 设备通信模拟器 | UC-X-01 | `tools/device_link_sim` 独立 CMake、固定数组帧编解码和非法帧测试 | 独立 `device_link_core_test`；S1～S4 的连接、5 桩并发和 GUI 仍待实现 | 部分完成：通信帧核心已实现 |
| 安卓用户端（已停止开发） | Android 用户端 | `apps/mobile` Qt Quick 用户端保留为迁移参考；自 2026-09-14 起不再继续开发，`tests/mobile` 中原依赖 `apps/user` Qt 源码的布局用例随之移除 | `ncs_mobile_contract` 与 `ncs_mobile_station_layout` 仍可独立运行（不在主 CMake 树内） | 已冻结：仅作为 UC-U-13 的迁移参考 |

## 非功能需求状态

| NFR 组 | 状态 | 证据 / 缺口 |
| --- | --- | --- |
| NFR-C-03、NFR-C-04、NFR-M-02、NFR-M-03、NFR-S-02~S-05、NFR-R-01、NFR-R-03 | 部分完成 | 原 SQLite 测试仅作历史证据；本轮已修复 OWNER 引导 TLS 校验、并发锁序、注销竞争、`.dump` 清理与备份保留统一 `now`，并补隔离恢复与保留边界测试。回归证据见「独立复核」节（CTest 43/43）。仍待独立环境验收：生产 CA 真实 `verify-full`、受限最小权限账户、代表性规模迁移与回滚演练、Ubuntu CI |
| NFR-M-01 | 部分完成 | `scripts/check.sh` 行数门禁带存量例外清单，大文件待拆分 |
| NFR-M-04 | 完成 | 结构化日志与脱敏已测；ops_log/device_command 180 天、outbox 7/30 天保留清理已实现并逐边界测试（含外键完整性门禁） |
| NFR-S-01 | 部分完成 | PBKDF2-HMAC-SHA256（600k 次迭代、版本化摘要）代替规格首选 Argon2id，偏差已在安全基线记录 |
| NFR-R-02 | 进行中 | PostgreSQL 连接失败统一脱敏，锁超时已配置；认证、证书、死锁、主备切换和连接池耗尽故障注入尚待补齐，SQLite 文件破坏测试不再适用 |
| NFR-U-01、NFR-U-02、NFR-U-03、NFR-C-01、NFR-C-02、NFR-D-01 | 部分完成 | Web 用户端断点、导航形态与失败提示由前端测试覆盖，缺真实浏览器验收截图；跨平台与路径重定位缺系统性验收；Windows CI 修复中 |
| NFR-M-05、NFR-S-06、NFR-S-07 | 部分完成 | Agent 依赖方向、只读边界与 Key 不出服务端由 `ncs_agent_*` 与前端构建产物检查覆盖；真实凭据下的端到端验收待配置 |
| NFR-P-06 | 未开始 | Agent 对话 60 秒预算与降级 3 秒预算尚无压测证据 |
| NFR-P-02、NFR-P-04、NFR-P-05 | 待 PostgreSQL 复测 | 原 SQLite 营收微基准与全量压测仅作历史参考；须在 PostgreSQL 18 上重跑 3000 账号/100 在线/50 排队/48 充电及 REST/WS 阈值后才能恢复完成状态 |
| NFR-P-01、NFR-P-03、NFR-D-02 | 未开始 | 客户端页面刷新 CPU 与严格单机部署未验收 |

## 维护规则

- 状态变化先更新需求矩阵，再同步本表；标"完成"必须同时给出产物与验证证据。
- 当前未修复的已知问题：[Web 用户端本机联调 CORS 问题](web-client-dev-cors.md)——`npm run dev` 的 `/api` 代理会带上浏览器 `Origin`，被服务端来源白名单拒绝（403），当前只能用 `NCS_CORS_ALLOWED_ORIGINS` 运行期规避，两个候选修复方案待确认。
- 当前已知验收欠账：Web 用户端真实浏览器验收截图、AI 助手真实大模型与腾讯 Key 端到端验收、管理端 UI、大屏前端、NFR-P-01/P-03 客户端页面性能、NFR-P-06 Agent 响应预算、NFR-D-02 严格单机部署验收。
