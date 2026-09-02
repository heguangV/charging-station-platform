# NCS 电动汽车充电桩应用管理平台

基于 C++17、Qt Widgets、Crow 和 SQLite 的充电桩管理教学项目。功能范围与验收标准统一以 SRS 为准。

## 文档

- [软件需求规格说明书](docs/01-requirements-specification.md)：唯一需求基线。
- [研发实施指南](docs/development-guide.md)：工程结构、研发阶段和完成流程。
- [数据库设计](docs/database-design.md)：物理模型、约束、事务和迁移。
- [REST / WebSocket 接口](docs/database-api.md)：客户端与服务端通信契约。
- [完整需求矩阵](docs/01需求矩阵-NCS充电桩管理平台.xls)：负责人、排期、状态、困难和验收待办。
- [腾讯地图接入](docs/tencent-map-setup.md)：本地配置和故障排查。
- [运行与运维手册](docs/operations-guide.md)：启动、检查、备份、恢复和清理。
- [发布指南](docs/release-guide.md)：发布门禁、步骤、回滚和说明模板。
- [工程基础追踪](docs/requirements-traceability.md)：阶段一产物与验证状态。

## 仓库结构

```text
.
├── apps/
│   ├── user/           Qt Widgets 用户端骨架
│   ├── admin/          Qt Widgets 管理端骨架
│   ├── dashboard/      Vue/ECharts 大屏预留目录
│   └── mobile/         远期 Android/QML 实验
├── server/             服务端协议与运行时边界
├── core/               公共错误、领域与应用边界
├── infrastructure/     配置、日志及后续外部适配器
├── tests/              CTest 基础与烟雾测试
├── ml/                 机器学习预留目录
├── scripts/            构建、测试、证书和清理脚本
├── docs/               需求、设计和接入文档
├── src/                旧学习原型（默认不构建）
└── CMakeLists.txt      CMake 工程入口
```

目标模块及其职责见[研发实施指南](docs/development-guide.md)。

## 环境与构建

开发环境遵循 SRS 的 `NFR-C-*`，使用 Qt 6.2.x、C++17、CMake 3.24+、Ninja 和 GCC 11+；基础 Qt 组件为 Core、Widgets、Network 和 Sql，Charts/WebEngine 按功能阶段启用。

```bash
cp .env.example .env
export QT_CMAKE=/path/to/Qt/6.2.x/gcc_64/bin/qt-cmake
./scripts/configure.sh dev
./scripts/build.sh dev
./scripts/test.sh dev
```

无桌面环境执行：

```bash
./scripts/smoke-test.sh
```

## 开发规范

- 开发前确认对应的 `UC-*`、`BR-*` 或 `NFR-*`，并选择相关设计文档。
- UI、Controller、Service 和数据访问层职责分离；客户端不得直接打开 SQLite。
- 源码使用 UTF-8 和 C++17；路径使用 Qt 跨平台 API；单个手写源文件不超过 400 行。
- 数据库访问参数化，跨表写操作使用事务和持久化幂等键；后台任务不得阻塞事件循环或直接操作 UI。
- 真实 `.env`、密钥、数据库、日志、备份、构建产物和个人 IDE 配置不得提交。
- 提交前运行相关测试、烟雾测试和 `git diff --check`；Pull Request 关联需求编号并写明验证结果。
- 分支职责、提交要求、PR 门禁和发布流程统一遵循[研发实施指南 §7](docs/development-guide.md#7-变更与协作)；禁止直接推送 `main` 或 `develop`。
- 当前只完成工程基础骨架；数据库、Crow 通信和业务页面的真实完成状态见[工程基础追踪](docs/requirements-traceability.md)。

## 许可证

[GNU General Public License v3.0](LICENSE)
