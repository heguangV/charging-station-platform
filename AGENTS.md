# NCS 仓库 Agent 规则

## 适用范围

本文件适用于整个仓库。子目录可通过更具体的 `AGENTS.md` 补充局部规则，但不得降低本文件的要求。

## 事实来源

只读取当前任务所需的文档：

- 需求、业务行为、指标和验收标准：`docs/01-requirements-specification.md`；
- 架构、交付阶段、测试和协作流程：`docs/development-guide.md`；
- SQLite 表、约束、事务和迁移：`docs/database-design.md`；
- REST DTO、错误码和 WebSocket 事件：`docs/database-api.md`；
- 腾讯地图配置和故障排查：`docs/tencent-map-setup.md`；
- 启动、检查、备份、恢复和数据清理：`docs/operations-guide.md`；
- 发布门禁、步骤、回滚和发布说明：`docs/release-guide.md`；
- 安全控制与漏洞处理：`docs/security-baseline.md`、`SECURITY.md`；
- 已知风险与第三方依赖：`docs/risk-register.md`、`docs/third-party-dependencies.md`；
- 阶段一状态：`docs/requirements-traceability.md`；完整任务状态：`docs/01需求矩阵-NCS充电桩管理平台.xls`。

需求规格说明书是唯一的需求基线。设计文档负责说明实现方式，不得覆盖或降低需求。

## 修改前检查

- 检查 `git status`、当前分支和相关差异，保留用户及其他成员已有的修改。
- 实现业务前，确认对应的 `UC-*`、`BR-*` 或 `NFR-*` 及其验收标准。
- 判断受影响的层次并只读取、修改相关文档，避免无关改写。
- 查看 `docs/requirements-traceability.md` 和需求矩阵中的真实状态；文档或占位目录存在不等于业务功能已经实现。
- 未决选择会实质改变业务行为或数据时，先向用户确认，不得默认采用更弱的需求。

## 工程边界

- 按研发实施指南分离 Qt UI、Crow Controller、应用服务、领域逻辑和基础设施。
- 正式可执行目标为 `ncs_user`、`ncs_admin` 和 `ncs_server`；公共目标为 `ncs_core` 和 `ncs_infrastructure`。`src/` 旧原型与 `apps/mobile/` 实验默认不构建，不得作为正式功能入口。
- 正式代码进入 `apps/user`、`apps/admin`、`server`、`core`、`infrastructure` 和 `tests`；不得重新建立 `client_user`、`client_admin` 或客户端数据访问层。
- 客户端只通过已定义的 REST/WebSocket 契约访问服务，不得直接打开 SQLite。
- 数据库访问必须参数化，并保持事务、幂等、线程归属和故障恢复约束。
- 金额和价格使用整数分，电量使用整数毫瓦时，时间使用 UTC Unix 秒；显示层才换算单位和本地时间。
- 不得阻塞 Qt 或 Crow 事件循环；后台工作线程不得直接操作 UI 对象。
- 不得暴露密钥、凭据、完整手机号、内部路径、SQL 或堆栈信息。
- 配置由进程环境变量覆盖指定 `.env` 文件；正式环境必须启用 HTTPS、禁用明文 HTTP 和模拟短信。真实 `.env`、开发私钥和地图 Key 不得进入 Git。
- 公开错误码只增不改，并同步接口文档、共享枚举和测试；日志统一包含 UTC 时间、级别、模块和请求 ID，并过滤敏感字段。
- 代码和文档修改应聚焦当前目标，不混入无关格式化或生成文件。

## 验证要求

- 执行足以覆盖本次改动的最小构建，以及相关单元、集成、契约、UI 或端到端测试。
- UI 改动应按需检查加载、空数据、失败和恢复状态。
- 数据库或协议改动应检查失败回滚、并发、幂等重试和兼容性。
- 可执行目标发生变化时运行烟雾测试；每次修改均运行 `git diff --check`。
- 提交前运行 `./scripts/check.sh`。脚本在本地检查相对 `HEAD` 的已暂存、未暂存和未跟踪
  C/C++ 文件；复现 Pull Request 检查时通过 `NCS_CHECK_BASE_REF=<目标基线>` 覆盖完整变更集。
- 批量运行 `clang-format` 前必须先把输入限制为 `.cpp`、`.h`，不得把 CMake、Python、文档或
  其他格式文件交给 C++ 格式化器。
- 工具链满足 Qt 6.2.x、CMake 3.24+、Ninja 和 GCC 11+ 时，优先执行：

```bash
./scripts/configure.sh dev
./scripts/build.sh dev
./scripts/test.sh dev
./scripts/smoke-test.sh
./scripts/check.sh
```

- 不得为了通过本机检查降低仓库的最低工具链版本；环境不满足时，明确报告差异，并使用不修改正式基线的隔离验证方式。
- 测试通过 CTest 注册真实可执行项并使用层级标签；不得创建空测试目标或用占位页面代替证据。
- 如实报告已运行的命令、检查结果及无法完成的验证。

## Git 与外部操作

- 遵循 `docs/development-guide.md` 第 7 节；不得直接或强制推送到 `main`、`develop`。
- 除非用户明确要求创建或切换分支，否则在当前个人分支工作。
- 未经用户明确要求，不得提交、推送、创建 Pull Request、合并、打标签或发布。
- 获得提交授权后，先复核暂存区差异，排除密钥、`.env`、数据库、日志、备份、构建产物、IDE 配置和无关修改。
- 不得改写共享历史或丢弃用户修改；破坏性 Git 操作必须获得明确授权。

## 文档维护

- 需求变化先修改需求规格说明书，再更新相关设计、代码和测试。
- 每项规则只在其负责文档中完整定义，其他文件通过链接引用，避免重复和冲突。
- README 只保留仓库导航、环境准备、构建运行和精简的协作入口。
- 需求、数据库、接口或任务状态变化后，同步其负责文档、`docs/requirements-traceability.md` 和完整需求矩阵；只有产物与验证证据同时存在时才能标记完成。

## 协作模板与格式

- Pull Request 使用 `.github/PULL_REQUEST_TEMPLATE.md` 的精简结构：关联信息、变更内容、影响与兼容性、验证、风险与回滚。
- Issue 使用 `.github/ISSUE_TEMPLATE/` 的表单；缺陷至少包含关联需求、问题说明、复现步骤、环境和脱敏证据。
- 运行手册保持“元信息、环境、启动、检查、备份恢复、故障、清理、记录”的章节顺序。
- 发布说明使用 `docs/release-guide.md` 的精简模板：基本信息、本次更新、升级影响、验证与处置。
- 需求矩阵保持已确认的九列结构：`NO.`、大分类、中分类、小分类、详细说明、负责人、预计日期、状态、困难。
- 未经用户再次确认，不增加重复栏目、冗长检查表或把上述模板恢复为更复杂格式。
