# NCS 电动汽车充电桩应用管理平台

基于 C++17、Qt Widgets、Crow 和 SQLite 的充电桩管理教学项目。功能范围与验收标准统一以 SRS 为准。

## 文档

- [软件需求规格说明书](docs/01-requirements-specification.md)：唯一需求基线。
- [研发实施指南](docs/development-guide.md)：工程结构、研发阶段和变更协作流程。
- [数据库设计](docs/database-design.md)：物理模型、约束、事务和迁移。
- [REST / WebSocket 接口](docs/database-api.md)：客户端与服务端通信契约。
- [完整需求矩阵](docs/01需求矩阵-NCS充电桩管理平台.xls)：负责人、排期、状态、困难和验收待办。
- [需求追踪](docs/requirements-traceability.md)：各交付阶段的产物与验证证据。
- [新增增强任务实施路径](docs/enhancement-tasks-implementation-plan.md)：拍照头像与独立设备通信模拟器的模块边界、顺序和门禁。
- [安全基线](docs/security-baseline.md)与[安全报告流程](SECURITY.md)。
- [腾讯地图接入](docs/tencent-map-setup.md)：本地配置和故障排查。
- [运行与运维手册](docs/operations-guide.md)：服务端配置、启动、检查、备份、恢复和清理。
- [发布指南](docs/release-guide.md)：发布门禁、步骤、回滚和说明模板。
- [风险登记](docs/risk-register.md)与[第三方依赖](docs/third-party-dependencies.md)。
- [变更记录](CHANGELOG.md)。

## 仓库结构

```text
.
├── apps/
│   ├── user/           Qt Widgets 用户端（登录/导航走真实服务端，业务数据暂由演示服务兜底）
│   ├── admin/          Qt Widgets 管理端骨架
│   ├── dashboard/      Vue/ECharts 大屏预留目录
│   └── mobile/         远期 Android/QML 实验
├── server/             Crow 服务端
│   ├── controller/     路由、DTO 转换与协议适配
│   ├── middleware/     鉴权、错误、限流与请求日志
│   ├── websocket/      WebSocket 接入、outbox 投递与进度推送
│   └── runtime/        配置、启动检查、周期调度与 ML 子进程管理
├── core/               不依赖 UI、Crow 或 SQLite 的核心层
│   ├── domain/         实体、值对象与错误码
│   ├── application/    用例、服务接口与权限边界
│   └── include/ncs/core/  公共 Result/Error 值类型
├── infrastructure/     外部能力实现
│   ├── sqlite/         数据库迁移、仓储、事务与备份
│   ├── map/            腾讯地理编码/路线规划与 Haversine 降级
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

目标模块及其职责见[研发实施指南](docs/development-guide.md)。

## 环境与构建

开发环境遵循 SRS 的 `NFR-C-*`：Qt 6.2+、C++17、CMake 3.24+、Ninja 和 GCC 11+；基础 Qt 组件为 Core、Widgets、Network、Sql 和 Charts。Crow 1.3.3 与 standalone Asio 1.30.2 优先使用已安装包，未安装时由 CMake 按固定版本标签获取。

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

无桌面环境执行烟雾测试，或运行全部测试（包含真实 HTTPS 启停烟雾测试）：

```bash
./scripts/smoke-test.sh
ctest --test-dir build/dev --output-on-failure
```

## 协作入口

- 开发前确认对应的 `UC-*`、`BR-*` 或 `NFR-*`，并选择相关设计文档；各模块当前状态以[完整需求矩阵](docs/01需求矩阵-NCS充电桩管理平台.xls)和[需求追踪](docs/requirements-traceability.md)为准。
- UI、Controller、Service 和数据访问层职责分离；客户端不得直接打开 SQLite，数据库访问全部参数化，金额用整数分、电量用整数毫瓦时、时间用 UTC Unix 秒。
- 源码使用 UTF-8 和 C++17；路径使用 Qt 跨平台 API；单个手写源文件不超过 700 行，存量例外清单见 `scripts/check.sh`。
- 后台任务不得阻塞 Qt 或 Crow 事件循环；不得暴露密钥、完整手机号、SQL 或堆栈信息。
- 真实 `.env`、密钥、数据库、日志、备份、构建产物和个人 IDE 配置不得提交。
- 提交前运行相关测试、烟雾测试和 `./scripts/check.sh`；Pull Request 使用仓库模板并关联需求编号。
- 分支职责、提交要求、PR 门禁和发布流程统一遵循[研发实施指南 §7](docs/development-guide.md#7-变更与协作)；禁止直接推送 `main` 或 `develop`。

## 许可证

[GNU General Public License v3.0](LICENSE)
