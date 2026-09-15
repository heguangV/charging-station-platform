# NCS 电动汽车充电桩应用管理平台

基于 C++17、Crow、PostgreSQL 18 与 Vue 3 的充电桩管理教学项目。车主端与管理端均为 Vue 3 + HTML5 响应式 Web（车主端同一套页面适配 PC 与手机浏览器，管理端为宽屏控制台），并包含独立的 AI 出行助手模块提供附近充电站、周边餐饮/咖啡 POI 与路线推荐。功能范围与验收标准统一以 SRS 为准。

## 文档

- [软件需求规格说明书](docs/01-requirements-specification.md)：唯一需求基线。
- [研发实施指南](docs/development-guide.md)：工程结构、研发阶段和变更协作流程。
- [数据库设计](docs/database-design.md)：物理模型、约束、事务和迁移。
- [REST / WebSocket 接口](docs/database-api.md)：客户端与服务端通信契约。
- [完整需求矩阵](docs/01需求矩阵-NCS充电桩管理平台.xls)：负责人、排期、状态、困难和验收待办。
- [需求追踪](docs/requirements-traceability.md)：各交付阶段的产物与验证证据。
- [新增增强任务实施路径](docs/enhancement-tasks-implementation-plan.md)：拍照头像与独立设备通信模拟器的模块边界、顺序和门禁。
- [安全基线](docs/security-baseline.md)与[安全报告流程](SECURITY.md)。
- [腾讯地图接入](docs/tencent-map-setup.md)：本地配置和故障排查（前端 JS Key 与服务端 Server Key 的分工）。
- [架构改造交接说明（2026-09-14）](docs/architecture-change-handoff-2026-09-14.md)：本轮 Web 化与 Agent 改造的现状、验证证据、待决事项与本机环境差异。
- [Web 用户端本机联调 CORS 问题](docs/web-client-dev-cors.md)：`npm run dev` 代理被服务端来源校验拒绝的现象、原因、临时规避与待决修复方案。
- [运行与运维手册](docs/operations-guide.md)：服务端配置、启动、检查、备份、恢复和清理。
- [发布指南](docs/release-guide.md)：发布门禁、步骤、回滚和说明模板。
- [风险登记](docs/risk-register.md)与[第三方依赖](docs/third-party-dependencies.md)。
- [变更记录](CHANGELOG.md)。

## 仓库结构

```text
.
├── apps/
│   ├── user/           Vue 3 + Vite 响应式 Web 车主端（PC 侧边栏 / 手机底部导航，npm 独立构建）
│   ├── admin/          Vue 3 + Vite Web 管理控制台（侧边导航 + 数据表格/图表，npm 独立构建）
│   ├── dashboard/      Vue/ECharts 大屏
│   └── mobile/         Qt Quick Android 实验端（已停止开发，仅作迁移参考，默认不构建）
├── agent/              AI 出行助手一级模块（AgentService、工具抽象、station/POI/route 工具）
├── server/             Crow 服务端
│   ├── controller/     路由、DTO 转换与协议适配（含 Agent Controller）
│   ├── middleware/     鉴权、错误、限流与请求日志
│   ├── websocket/      WebSocket 接入、outbox 投递与进度推送
│   └── runtime/        配置、启动检查、周期调度与 ML 子进程管理
├── core/               不依赖 UI、Crow 或 PostgreSQL 的核心层
│   ├── domain/         实体、值对象与错误码
│   ├── application/    用例、服务接口与权限边界（含 LLM/POI 等外部能力端口）
│   └── include/ncs/core/  公共 Result/Error 值类型
├── infrastructure/     外部能力实现
│   ├── database/       仓储工厂与组合端口
│   ├── postgres/       PostgreSQL 迁移、仓储、事务与备份
│   ├── sqlite/         仅测试/历史数据转换期间保留
│   ├── ai/             OpenAI-compatible 大模型客户端与配置（API Key 只在服务端）
│   ├── map/            腾讯 WebService 客户端、地理编码、POI、路线规划与 Haversine 降级
│   ├── files/          结构化日志、原子快照与模型产物
│   ├── config/         .env 环境文件加载
│   └── logging/        应用日志器
├── ml/                 Python 训练与预测管线
├── scripts/            构建、测试、证书和清理脚本
├── tests/              单元、数据库、契约、集成与烟雾测试
├── docs/               需求、设计和接入文档
├── src/                旧学习原型（默认不构建）
├── CMakeLists.txt      CMake 工程入口
├── LICENSE             GPL-3.0
└── README.md           仓库入口
```

模块依赖方向固定为 `server → agent → core / infrastructure`；`core` 与 `infrastructure` 不得依赖 `agent`，Agent 不得直接访问 PostgreSQL。

### 地图与大模型的分工

| 位置 | 能力 | 凭据 |
| --- | --- | --- |
| `apps/user`（浏览器） | 腾讯地图 **JavaScript API**：底图、站点 Marker、定位居中 | 只持有受来源限制的 `TENCENT_MAP_JS_KEY` |
| `apps/admin`、`apps/dashboard`（浏览器） | 管理控制台与运营大屏，只读/运维 REST | 不持有任何密钥 |
| `infrastructure/map`（服务端） | 腾讯地图 **WebService**：地理编码、POI 检索、路线规划 | 只持有 `TENCENT_MAP_SERVER_KEY` |
| `infrastructure/ai`（服务端） | OpenAI-compatible 大模型对话与 Tool Calling | 只持有 `AI_API_KEY` |

浏览器端不得持有 Server Key 或 AI Key；两者只从进程环境变量或本地 `.env` 读取。

目标模块及其职责见[研发实施指南](docs/development-guide.md)。

## 环境与构建

开发环境遵循 SRS 的 `NFR-C-*`：Qt 6.2+、C++17、CMake 3.24+、Ninja 和 GCC 11+；C++ 侧只剩服务端与公共库（Qt 组件 Core、Widgets、Network、Sql）。Crow 1.3.3 与 standalone Asio 1.30.2 优先使用已安装包，未安装时由 CMake 按固定版本标签获取。三个 Web 前端使用 Node.js 22+ 与 npm，独立于 CMake C++ 流程。

推荐使用工程脚本配置、构建和测试（构建目录为 `build/dev`）：

```bash
cp .env.example .env
export QT_CMAKE=/path/to/Qt/6.2.x/gcc_64/bin/qt-cmake
./scripts/configure.sh dev
./scripts/build.sh dev
./scripts/test.sh dev
```

单独构建并启动 Crow 服务端：

```bash
cmake --build build/dev --target ncs_server
./scripts/generate-dev-cert.sh
./build/dev/server/ncs_server \
  --tls-certificate runtime/certs/dev-cert.pem \
  --tls-private-key runtime/certs/dev-key.pem
```

服务默认只在 `https://127.0.0.1:8443` 监听；本机 HTTP/WS 联调需显式启用受限回环模式，完整的服务端启动参数、环境变量、健康检查、日志、备份恢复和数据清理见[运行与运维手册](docs/operations-guide.md)。开发证书仅用于本机，`runtime/certs/`、`secrets/`、`*.pem` 和 `*.key` 已被 Git 忽略，不得提交私钥。

构建并运行 Web 前端（独立于 CMake，没有对应的 C++ 可执行目标）：

```bash
cd apps/user        # 车主端，dev 默认 http://localhost:5173
npm install && npm run dev

cd apps/admin       # 管理端，dev 默认 http://localhost:5174
npm install && npm run dev

npm run build       # 生成对应应用目录下的 dist/
npm run test        # vitest 前端测试
```

三个前端的 `dev` 都把 `/api` 代理到 `ncs_server`（默认 `https://127.0.0.1:8443`，可用 `VITE_NCS_API_TARGET` 覆盖）。**本机用 `npm run dev` 联调时必须给服务端放行来源**，否则浏览器请求会被来源校验拒绝（403），原因与两个待决修复方案见[Web 用户端本机联调 CORS 问题](docs/web-client-dev-cors.md)。

车主端的 `npm run dev` 与 `npm run build` 会从仓库根 `.env` 读取 `TENCENT_MAP_JS_KEY` 注入前端（`envDir` 指向仓库根，仅放行 `VITE_*` 与 `TENCENT_MAP_JS_KEY`）。`TENCENT_MAP_SERVER_KEY`、`AI_API_KEY` 永不进入任何前端产物，管理端与大屏也不读取任何密钥。

启用 AI 出行助手需要在 `.env` 中填写 `AI_PROVIDER`、`AI_MODEL` 与 `AI_API_KEY`（可选 `AI_BASE_URL`、`AI_TIMEOUT_MS`）；未填写时助手自动退化为确定性的工具检索结果，不会使对话失败。

无桌面环境执行烟雾测试，或运行全部测试（包含真实 HTTPS 启停烟雾测试）：

```bash
./scripts/smoke-test.sh
ctest --test-dir build/dev --output-on-failure
```

## 协作入口

- 开发前确认对应的 `UC-*`、`BR-*` 或 `NFR-*`，并选择相关设计文档；各模块当前状态以[完整需求矩阵](docs/01需求矩阵-NCS充电桩管理平台.xls)和[需求追踪](docs/requirements-traceability.md)为准。
- UI、Controller、Service 和数据访问层职责分离；客户端不得直连 PostgreSQL，数据库访问全部参数化，金额用整数分、电量用整数毫瓦时、时间用 UTC Unix 秒。
- 三个 Web 前端共用同一套设计令牌与动效层；动效必须提供 `prefers-reduced-motion: reduce` 降级，且不得改变文案、数值与字段取值。
- 用户端为单一响应式页面集合（不得建第二套移动端页面）；Agent 只读、不得绕过应用服务，模型与地图厂商协议只出现在 `infrastructure/`。
- 源码使用 UTF-8 和 C++17；路径使用 Qt 跨平台 API；单个手写源文件不超过 700 行，存量例外清单见 `scripts/check.sh`。
- 后台任务不得阻塞 Qt 或 Crow 事件循环；不得暴露密钥、完整手机号、SQL 或堆栈信息。
- 真实 `.env`、密钥、数据库、日志、备份、构建产物和个人 IDE 配置不得提交。
- 提交前运行相关测试、烟雾测试和 `./scripts/check.sh`；Pull Request 使用仓库模板并关联需求编号。
- 分支职责、提交要求、PR 门禁和发布流程统一遵循[研发实施指南 §7](docs/development-guide.md#7-变更与协作)；禁止直接推送 `main` 或 `develop`。

## 许可证

[GNU General Public License v3.0](LICENSE)
