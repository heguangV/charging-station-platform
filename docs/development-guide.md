# NCS 研发实施指南

| 项目 | 内容 |
| --- | --- |
| 用途 | 规定工程拆分、研发顺序、质量门禁和完成条件 |
| 需求基线 | [软件需求规格说明书](01-requirements-specification.md) |
| 版本 | 1.3 |
| 更新日期 | 2026-09-14 |

本文不重复业务规则、数值指标或验收条款。功能实现直接引用 SRS 的 `UC-*`、`BR-*` 和 `NFR-*`；数据库与通信细节分别引用[数据库设计](database-design.md)和[接口文档](database-api.md)。

## 1. 文档职责

| 文档 | 唯一职责 |
| --- | --- |
| `01-requirements-specification.md` | 定义系统必须实现的功能、规则、指标和验收结果 |
| `development-guide.md` | 定义如何组织工程、安排研发和判断任务完成 |
| `database-design.md` | 定义 PostgreSQL 物理模型、约束、事务和迁移 |
| `database-api.md` | 定义 HTTPS REST / WebSocket 通信契约（含 §5A Agent 对话接口） |
| `tencent-map-setup.md` | 定义腾讯地图本地配置与排错步骤 |
| `enhancement-tasks-implementation-plan.md` | 定义 `UC-U-11`、`UC-X-01` 的模块边界、实施顺序、产物和验证门禁 |
| `database-milestone-implementation-plan.md` | 定义数据库里程碑六条工作流的模块边界、文件归属、实施顺序与验证门禁 |

若设计文档与 SRS 冲突，以 SRS 为准；设计文档不得新增未经需求支持的强制业务行为。

## 2. 架构落地

```text
Vue/HTML5 车主端（PC + 手机同一套页面）─┐
Vue/HTML5 管理端（宽屏控制台）──────────┼── HTTPS REST / WebSocket ── Crow 服务端 ── PostgreSQL
Vue 数据大屏 ──────────────────────────┘                                  │
                                                                          ├── Core 业务服务
                                                                          ├── Agent（LLM + Tool Calling）
                                                                          ├── 腾讯地图 WebService（地理编码 / POI / 路线）
                                                                          ├── 大模型服务（OpenAI-compatible）
                                                                          └── Python ML 子进程
```

- 三个 Web 前端（车主端、管理端、大屏）负责交互和展示，不保存服务端业务真相；它们只通过已定义的 REST/WebSocket 契约取数，且共用同一套设计令牌与动效层。
- Crow Controller 只处理协议、鉴权入口和 DTO 转换；业务规则位于应用服务层；Agent Controller 同样只做 HTTP、鉴权、DTO 转换与参数校验。
- 领域层不依赖 UI 框架、Crow、PostgreSQL 或外部地图；`core` 与 `infrastructure` 不得依赖 `agent`。
- Agent 是根目录一级模块，依赖方向固定为 `server → agent → core / infrastructure`：站点能力复用 `core/application` 的应用服务，地图与 LLM 通过 core 端口注入，具体厂商协议只出现在 `infrastructure/map` 与 `infrastructure/ai`。
- 腾讯地图分工：前端 JavaScript API 只负责地图显示（只持有受来源限制的 JS Key），服务端 WebService 只负责地理编码、POI 与路线规划（Server Key 只在服务端进程）。
- PostgreSQL 仅由服务端数据访问层连接；Agent 与 ML 都不得直接访问数据库，ML 通过内部接口交换数据。
- 实时事件用于及时更新，REST 快照用于首次加载和断线恢复。

## 3. 目标工程结构

```text
apps/
├── user/                  Vue 3 + Vite 响应式 Web 车主端（PC 与手机同一套页面，npm 独立构建）
├── admin/                 Vue 3 + Vite Web 管理控制台（宽屏优先，npm 独立构建）
├── dashboard/             Vue/ECharts 大屏
└── mobile/                Qt Quick Android 实验端（已停止开发，仅作迁移参考，默认不构建）
agent/                     AI 出行助手一级模块（AgentService、工具抽象、station/POI/route 工具）
server/
├── controller/            Crow 路由和 DTO 转换（含 Agent Controller）
├── middleware/            鉴权、错误、限流和日志
└── runtime/               配置、计费、调度、到期和通知
core/
├── domain/                实体、值对象和状态机
├── application/           用例、服务接口和权限边界（含 LLM/POI 等外部能力端口）
└── include/ncs/core/      公共 Result/Error 值类型
infrastructure/
├── database/              仓储组合端口与后端工厂
├── postgres/              schema、迁移、仓储、事务和备份
├── sqlite/                仅测试/历史数据转换期间保留，不进入正式服务链接
├── config/                环境配置加载与校验
├── logging/               应用日志、请求 ID 和脱敏
├── ai/                    OpenAI-compatible 大模型客户端与配置
├── map/                   腾讯 WebService 客户端、地理编码、POI、路线与本地距离降级
└── files/                 头像和大屏快照
ml/                        训练、预测和评估
tests/                     单元、集成、契约、并发、UI 和端到端测试
docs/                      需求、设计和接入文档
```

目录建立后同步更新根 README；规划目录不存在时不得写成已有实现。

## 4. 研发阶段

| 阶段 | 工作范围 | 主要交付物 | 退出条件 |
| --- | --- | --- | --- |
| 1. 工程基础 | CMake 目标、公共库、配置、日志、错误和测试框架 | 可启动的用户端、管理端和服务端骨架 | Ubuntu 构建、启动和烟雾测试通过 |
| 2. 数据与领域 | schema、迁移、种子、仓储、状态机和事务 | 数据层、领域服务、数据库测试 | 初始化、升级、并发和回滚测试通过 |
| 3. 服务端通信 | REST、WebSocket、会话、调度、模拟计费和恢复 | Crow 服务、契约测试、虚拟客户端 | 鉴权、幂等、断线恢复和持续计费通过 |
| 4. 用户端 | 实现 `UC-U-*` | 完整竖屏用户流程 | 所有用户用例正常流与异常流通过 |
| 5. 管理端 | 实现 `UC-A-*` | 管理页面、图表和运维操作 | 权限、管理操作和错误处理通过 |
| 5A. 管理端 Web 化 | `UC-A-01`~`UC-A-09` 的界面形态迁移 | `apps/admin`（Vue 3 + Vite + ECharts）替代原 Qt 管理端，共用车主端设计语言 | 管理端 npm 构建与前端测试通过；管理接口契约仍由 `ncs_admin_routes`、`ncs_admin_account_routes` 覆盖 |
| 6. 大屏与 ML | 实现 `UC-W-*`、`UC-M-*` 及地图 | 大屏、地图、训练和预测产物 | 对应用例及模型验收通过 |
| 7. 系统验收 | 验证全部 `NFR-*` 和验收清单 | 测试报告、恢复记录、截图和运行说明 | SRS 追踪矩阵无未验证条目 |
| 8. 新增增强任务 | `UC-U-11` 拍照头像与 `UC-X-01` 独立设备通信模拟器 | 用户端可选摄像头能力、独立模拟器子工程及自动化证据 | 专项实施路径的 A0～A5、S0～S4 门禁全部通过 |
| 9. 响应式 Web 用户端与 Agent | `UC-U-13` 统一响应式 Web 用户端、`UC-U-14` AI 出行助手 | `apps/user`（Vue 3 + Vite）、`agent/`、`infrastructure/ai`、`infrastructure/map` 扩展、Agent Controller 与专项测试 | Web 端 npm 构建与前端测试通过；Agent 契约、工具调度与降级路径测试通过；地图与 LLM 均在无 Key 时仍能降级 |

阶段内优先完成可端到端验证的纵向切片，避免长期维护只有接口没有调用方的半成品。

阶段八的模块边界、实施顺序和验证门禁见[新增增强任务实施路径](enhancement-tasks-implementation-plan.md)。`UC-U-11` 必须在 `UC-U-05` 真实头像 REST 闭环后接入；`UC-X-01` 保持独立 CMake，不改变阶段三 WebSocket 契约或阶段五远程重启语义。

## 5. 单项任务流程

1. 选择需求编号并抄录其验收标准，不在任务中重新解释需求。
2. 明确受影响的领域对象、接口、数据表、权限、状态变化和失败路径。
3. 先增加领域测试或契约测试，再实现应用服务和基础设施。
4. 接入 Controller、客户端或大屏，补充集成与界面验证。
5. 执行相关测试、烟雾测试和静态格式检查。
6. 更新追踪关系和受影响的设计文档，通过代码审查后合并。

## 6. 测试分层

| 层级 | 关注点 |
| --- | --- |
| 单元测试 | 值对象、公式、状态机和纯领域规则 |
| 数据库测试 | schema、迁移、约束、事务、幂等和恢复 |
| 契约测试 | HTTP 方法、路径、DTO、错误码、鉴权和 WebSocket 事件 |
| 集成测试 | 服务端与 PostgreSQL、地图降级、ML 子进程和文件服务 |
| UI 测试 | 页面状态、尺寸、导航、输入校验和错误提示 |
| 端到端测试 | 用户与管理端通过真实 API 完成关键业务流程 |
| Agent 测试 | 工具调度与锚点注入、工具参数校验、大模型与地图失败降级、Agent Controller 契约 |
| 前端测试 | Web 用户端的响应式布局、定位与地图状态、API 解包与错误路径、Agent 结构化结果渲染 |
| 非功能测试 | SRS 规定的性能、容量、安全、备份和恢复指标 |

测试数据库、日志、模型和截图使用隔离目录，不得污染开发或演示数据。

PostgreSQL 集成门禁由 Ubuntu CI 安装 PostgreSQL 18 与 QPSQL 驱动后执行；设置
`NCS_REQUIRE_POSTGRES_TESTS=1`，缺少工具或版本不符时失败，不允许静默跳过。
`ncs_postgres_repository` 包含隔离库实际恢复校验；Windows 作源码构建及其余契约验证，
不以 Windows 跳过的 Unix 临时数据库测试作为 PostgreSQL 验收证据。

Web 前端不作为 CMake 目标：改动后在对应的 `apps/user`、`apps/admin` 或 `apps/dashboard`
执行 `npm install`、`npm run test` 与 `npm run build`，三者是前端改动的完成条件。
`scripts/check.sh` 的 C/C++ 检查范围包含 `agent/`；CI 通过
`NCS_CHECK_BASE_REF` 传入 Pull Request 或推送前的基线提交，检查该变更集。历史格式债不得阻断
无关变更，但新改动必须符合仓库 `.clang-format` 和 `NFR-M-01`。

## 7. 变更与协作

### 7.1 分支职责

| 分支 | 用途 | 写入方式 |
| --- | --- | --- |
| `main` | 已审核、已测试的正式基线 | 只接受来自 `develop` 或紧急修复分支的 Pull Request |
| `develop` | 团队集成与回归测试 | 只接受个人开发分支的 Pull Request |
| `feature/<账号>/<主题>` | 功能开发 | 从最新 `develop` 创建 |
| `fix/<账号>/<主题>` | 普通缺陷修复 | 从最新 `develop` 创建 |
| `docs/<账号>/<主题>` | 文档调整 | 从最新 `develop` 创建 |
| `hotfix/<账号>/<主题>` | 正式基线紧急修复 | 从 `main` 创建，合并后同步回 `develop` |

主题使用简短的小写英文和连字符。禁止直接推送或强制推送到 `main`、`develop`；不得删除受保护分支。

### 7.2 开发与提交

1. 开始任务前拉取远程状态，从最新目标基线创建个人分支。
2. 一个提交只表达一个清晰目的；提交标题建议使用 `feat:`、`fix:`、`docs:`、`test:`、`refactor:` 或 `chore:` 前缀。
3. 提交前检查 `git status` 和暂存区差异，不混入无关格式化、生成文件、本地配置或他人未完成修改。
4. 数据库迁移、公开 DTO、路由、错误码和 WebSocket 事件变更，先更新对应设计文档和契约测试。
5. 需求变化先修改 SRS，再修改设计、代码和测试；不得只在实现、提交说明或聊天记录中改变行为。
6. 个人分支已公开后避免改写共享历史；确需重写时必须先与协作者确认。

### 7.3 Pull Request

日常开发向 `develop` 提交 Pull Request。PR 必须包含：

- 关联的需求、问题或任务编号；
- 背景、主要变更和明确的不包含范围；
- 受影响的模块、数据、接口和兼容性；
- 实际执行的构建、测试及结果；
- UI 变化所需截图，故障修复所需复现与回归证据；
- 数据库迁移、配置变化、部署步骤和回滚方式（适用时）。

每个 PR 聚焦一个可验证目标。合并前必须满足：

- 至少一名其他成员批准；
- 审核意见已处理、讨论已解决；
- 必需状态检查、构建和测试通过；
- 分支与目标基线不存在未解决冲突；
- 文档和契约已与实现同步。

建议在 GitHub 为 `main` 和 `develop` 启用分支保护：要求 PR、至少一次批准、解决全部讨论、通过必需状态检查，并禁止强制推送和删除。

### 7.4 集成与发布

1. 个人分支通过审核后合入 `develop`。
2. 在 `develop` 执行功能、回归、集成和必要的非功能测试；失败时回到个人分支修复并重新审核。
3. 满足发布条件后，以 Pull Request 将 `develop` 合入 `main`，附测试结论、已知限制和回滚说明。
4. 紧急修复合入 `main` 后必须同步到 `develop`，避免后续版本重新引入问题。

```text
feature / fix / docs ──PR + Review──> develop ──集成与回归──> main
hotfix ────────────────PR + Review──────────────────────────> main
  └────────────────────────────同步─────────────────────────> develop
```

## 8. 完成定义

单项需求完成必须同时满足：

- 可追踪到明确的 SRS 编号；
- 正常流、异常流、权限和边界条件均有测试证据；
- 分层、线程、事务和接口约束符合对应设计文档；
- 相关构建、测试、烟雾测试和 `git diff --check` 通过；
- 文档、配置示例和追踪关系已同步。

项目完成条件仅以 SRS 的需求追踪矩阵、非功能指标和验收清单为准。
