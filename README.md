# NCS 电动汽车充电桩应用管理平台

基于 C++17、Qt Widgets、Crow 和 SQLite 的充电桩管理教学项目。功能范围与验收标准统一以 SRS 为准。

## 文档

- [软件需求规格说明书](docs/01-requirements-specification.md)：唯一需求基线。
- [研发实施指南](docs/development-guide.md)：工程结构、研发阶段和完成流程。
- [数据库设计](docs/database-design.md)：物理模型、约束、事务和迁移。
- [REST / WebSocket 接口](docs/database-api.md)：客户端与服务端通信契约。
- [腾讯地图接入](docs/tencent-map-setup.md)：本地配置和故障排查。

## 仓库结构

```text
.
├── apps/mobile/        远期 Android/QML 地图实验
├── docs/               需求、设计和接入文档
├── src/                当前 Qt Widgets 原型
├── CMakeLists.txt      CMake 工程入口
├── LICENSE             GPL-3.0
└── README.md           仓库入口
```

目标模块及其职责见[研发实施指南](docs/development-guide.md)。

## 环境与构建

开发环境遵循 SRS 的 `NFR-C-*`，当前仓库使用 Qt 6.2、C++17、CMake 3.24+、Ninja 和 GCC 11+；Qt 组件包括 Widgets、Network、Sql 和 Charts。

```bash
/path/to/Qt/6.2.0/gcc_64/bin/qt-cmake -S . -B build -G Ninja
cmake --build build --target codex-qt-demo
./build/codex-qt-demo
```

无桌面环境执行：

```bash
QT_QPA_PLATFORM=offscreen ./build/codex-qt-demo --smoke-test
```

## 开发规范

- 开发前确认对应的 `UC-*`、`BR-*` 或 `NFR-*`，并选择相关设计文档。
- UI、Controller、Service 和数据访问层职责分离；客户端不得直接打开 SQLite。
- 源码使用 UTF-8 和 C++17；路径使用 Qt 跨平台 API；单个手写源文件不超过 400 行。
- 数据库访问参数化，跨表写操作使用事务和持久化幂等键；后台任务不得阻塞事件循环或直接操作 UI。
- 真实 `.env`、密钥、数据库、日志、备份、构建产物和个人 IDE 配置不得提交。
- 提交前运行相关测试、烟雾测试和 `git diff --check`；Pull Request 关联需求编号并写明验证结果。
- 分支职责、提交要求、PR 门禁和发布流程统一遵循[研发实施指南 §7](docs/development-guide.md#7-变更与协作)；禁止直接推送 `main` 或 `develop`。

## 许可证

[GNU General Public License v3.0](LICENSE)
