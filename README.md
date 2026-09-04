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
│   ├── user/           Qt Widgets 用户端（当前为模拟数据骨架）
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
├── tests/              单元、集成、契约与烟雾测试
├── docs/               需求、设计和接入文档
├── src/                旧学习原型（默认不构建）
├── CMakeLists.txt      CMake 工程入口
├── LICENSE             GPL-3.0
└── README.md           仓库入口
```

目标模块及其职责见[研发实施指南](docs/development-guide.md)。

## 环境与构建

开发环境遵循 SRS 的 `NFR-C-*`，当前仓库使用 Qt 6.2+、C++17、CMake 3.22+、Ninja 和 GCC 11+；基础 Qt 组件为 Core、Widgets、Network、Sql 和 Charts。Crow 1.3.3 与 standalone Asio 1.30.2 优先使用已安装包，未安装时由 CMake 按固定版本标签获取。

推荐使用工程脚本配置、构建和测试（构建目录为 `build/dev`）：

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

单独构建并启动 Crow 服务端：

```bash
cmake --build build/dev --target ncs_server
./scripts/generate-dev-cert.sh
./build/dev/ncs_server \
  --tls-certificate runtime/certs/dev-cert.pem \
  --tls-private-key runtime/certs/dev-key.pem
```

上述开发证书仅用于本机，`runtime/certs/`、`secrets/`、`*.pem` 和 `*.key` 已被 Git 忽略，不得提交私钥。服务默认只在 `https://127.0.0.1:8443` 监听；仅在显式启用 `NCS_ALLOW_INSECURE_HTTP=true`、环境为 `development` 且监听数字回环地址时，才使用本机 HTTP/WS 联调模式。测试、验收和生产环境仍强制 HTTPS/WSS。所有 REST 路由必须通过受控注册器挂载到 `/api/v1`。`GET /api/v1/system/health/live` 用于进程存活检查；`ready` 检查 SQLite schema、读写能力、WAL 和迁移版本，任一检查失败均返回 HTTP 503。

服务端在监听前会检查证书时间、SAN 中的监听 IP、证书与私钥匹配关系、Unix 私钥权限及端口可用性。任一检查失败都以配置错误退出，不进入 Crow 事件循环。`SIGINT` 和 `SIGTERM` 会停止接收新连接、关闭 I/O 事件循环并以退出码 0 结束，正常退出后可立即重启。

### 服务端启动配置
### 服务端启动配置

命令行参数优先于同名进程环境变量，进程环境变量优先于环境文件；均未设置时使用受控的本机开发默认值。

| 配置 | 命令行 | 环境变量 | 开发默认值 |
| --- | --- | --- | --- |
| 运行环境 | `--environment` | `NCS_ENVIRONMENT` | `development` |
| 监听 IP | `--listen-address` | `NCS_LISTEN_ADDRESS` | `127.0.0.1` |
| 监听端口 | `--port` | `NCS_PORT` | `8443` |
| Crow 工作线程 | `--worker-threads` | `NCS_WORKER_THREADS` | `2` |
| 充电时间倍率 | `--charge-time-scale` | `NCS_CHARGE_TIME_SCALE` | `60` |
| 日志级别 | `--log-level` | `NCS_LOG_LEVEL` | `info` |
| 日志目录 | `--log-directory` | `NCS_LOG_DIRECTORY` | `logs/` |
| SQLite 数据库 | `--database-path` | `NCS_DATABASE_PATH` | `data/charge_platform.db` |
| TLS 证书 | `--tls-certificate` | `NCS_TLS_CERTIFICATE` | `secrets/ncs-dev-cert.pem` |
| TLS 私钥 | `--tls-private-key` | `NCS_TLS_PRIVATE_KEY` | `secrets/ncs-dev-key.pem` |
| 本机开发 HTTP | `--allow-insecure-http` | `NCS_ALLOW_INSECURE_HTTP` | `false` |
| Dashboard 快照 | `--dashboard-snapshot` | `NCS_DASHBOARD_SNAPSHOT` | `apps/dashboard/public/data/dashboard.json` |
| Python 解释器 | `--python-executable` | `NCS_PYTHON_EXECUTABLE` | `python3` |
| ML 工作脚本 | `--ml-worker-script` | `NCS_ML_WORKER_SCRIPT` | `ml/worker.py` |
| ML 活动模型 | `--ml-model-path` | `NCS_ML_MODEL_PATH` | `ml/models/load_rf.pkl` |
| 腾讯地图服务端 Key | `--tencent-map-key` | `NCS_TENCENT_MAP_KEY` | 空（地图能力降级） |

`--environment` 允许 `development`、`test`、`acceptance`、`production`。开发模式会启用演示凭据，因此只允许监听回环地址；监听地址还必须是数字 IP，通配地址、多播地址和非法地址会在启动前被拒绝。HTTP 模式还要求显式开关、`development` 和数字回环地址三项同时满足，并输出 WARNING；否则启动失败。未启用 HTTP 时，证书或私钥缺失、不可读或指向同一文件会在监听前失败。未显式配置的文件路径以服务程序所在部署目录为稳定基准（源码构建会自动定位项目资源），不随 shell 当前目录漂移。执行 `./build/ncs_server --help` 查看完整参数。

环境文件：`NCS_ENV_FILE` 指定的文件（必须存在且可读）或资产目录下的 `.env`（存在时加载）提供可由进程环境变量覆盖的默认值；条目名与上表环境变量一致，仅腾讯地图服务端 Key 写作 `TENCENT_MAP_SERVER_KEY`。用户端地图另读取 `TENCENT_MAP_JS_KEY` 和受限来源 `TENCENT_MAP_JS_ORIGIN`，不会保留 Server Key。`.env` 为 Git 忽略的仅本机文件（权限 600），真实 Key 不得提交；可运行 `./scripts/configure-local-map.sh` 安全写入并以 `--check` 验证，详见 `docs/tencent-map-setup.md`。

### 结构化日志

应用日志按 UTC 日期写入 `logs/ncs_YYYYMMDD.log`，每行是一个 JSON 对象，固定包含 `timestamp`、`level`、`module`、`requestId`、`message`。应用日志保留 30 天；Unix 日志文件权限设为 `0600`。Crow 内部日志已接入同一输出，URL 查询串和 Bearer 值会先脱敏。

HTTP 请求会校验或生成 UUID `X-Request-ID`，并将同一值写入响应和请求日志。日志写入前统一过滤查询凭据、Bearer Token、手机号、验证码、密码、钱包/余额和订单字段；非请求启动日志的 `requestId` 为空字符串。审计/运维日志的 180 天保留将由独立业务能力实现，不混入普通应用日志。

### 全局异常响应

Crow 路由未捕获异常统一通过公共响应封装返回 HTTP 500 和 `INTERNAL_ERROR`（`code=13`），并设置 `Cache-Control: no-store`。响应和异常事件日志都不记录异常原文，避免泄露 SQL、内部路径、堆栈或令牌；请求 ID 会从请求作用域写入响应。

### 公共 HTTP 安全策略

普通 JSON、ML 批量和头像请求体上限分别为 1 MiB、8 MiB 和 5 MiB；普通接口截止时间 10 秒，统计接口 30 秒。默认限流支持持续 20 请求/秒和 50 请求突发，超限响应携带 `Retry-After`。跨域来源默认全部拒绝，只有显式加入运行时白名单的来源才会获得限定方法和请求头的 CORS 响应；同源 Qt/服务端调用不受影响。TLS 最低版本为 1.2，仅启用 AEAD 密码套件。

Dashboard 每 30 秒生成受权完整快照，并原子写入 `apps/dashboard/public/data/dashboard.json` 供断线降级；该文件应由 Crow 的鉴权路径提供，不得配置成匿名静态资源。路径可用 `--dashboard-snapshot` 覆盖。

ML 子进程默认使用 `python3 ml/worker.py`，依赖安装见 `ml/requirements.txt`。可通过 `--python-executable`、`--ml-worker-script` 和 `--ml-model-path` 覆盖；任务令牌只经子进程标准输入传递，内部接口仅接受回环来源。

密码使用版本化 PBKDF2-HMAC-SHA256 专用摘要；Token 至少包含 256 bit 随机值且服务端状态只保存 SHA-256 摘要。用户会话最多 3 个终端且不超过 30 天，管理员最多 2 个终端，Dashboard 会话不超过 8 小时并在空闲 30 分钟后失效。验证码有效 10 分钟、最多错误 5 次、生成冷却 60 秒，只有受限开发模式可得到模拟验证码。

### 用户身份接口

`/api/v1/user` 下已提供短信验证码、用户名密码注册、密码/短信登录、退出、会话查询与撤销、个人资料、昵称更新、头像上传与条件读取、凭据更新和账号注销接口。头像不会原样保存：服务端验证真实 PNG/JPEG/BMP 格式和 4096×4096 尺寸上限，绘制到新图像后统一编码为 PNG，以剥离上传元数据。

正式服务使用 `infrastructure/sqlite/sqlite_repository.*` 持久化用户、凭据、头像、钱包、站点、电桩、价格、排队、流程、订单、幂等结果和通知 outbox；每个调用线程独立打开连接，事务内的嵌套仓储操作复用同一连接。写接口的业务变化与幂等完成结果同事务提交，结算成功通知与订单、钱包和设备状态同事务写入。`core/application/in_memory_user_account_repository.*` 和 `InMemoryChargingRepository` 仅保留给快速单元与契约测试。服务端会话当前仍采用进程内安全存储，服务重启后客户端需要重新登录。

无桌面环境执行：

```bash
QT_QPA_PLATFORM=offscreen ./build/codex-qt-demo --smoke-test
```

运行全部服务端测试（包含真实 HTTPS 启停烟雾测试）：

```bash
ctest --test-dir build-ncs --output-on-failure
```


## 开发规范

- 开发前确认对应的 `UC-*`、`BR-*` 或 `NFR-*`，并选择相关设计文档。
- UI、Controller、Service 和数据访问层职责分离；客户端不得直接打开 SQLite。
- 源码使用 UTF-8 和 C++17；路径使用 Qt 跨平台 API；单个手写源文件不超过 700 行，存量例外清单见 `scripts/check.sh`。
- 数据库访问参数化，跨表写操作使用事务和持久化幂等键；后台任务不得阻塞事件循环或直接操作 UI。
- 真实 `.env`、密钥、数据库、日志、备份、构建产物和个人 IDE 配置不得提交。
- 提交前运行相关测试、烟雾测试和 `./scripts/check.sh`；该脚本只严格检查当前变更集，Pull Request 关联需求编号并写明验证结果。
- 分支职责、提交要求、PR 门禁和发布流程统一遵循[研发实施指南 §7](docs/development-guide.md#7-变更与协作)；禁止直接推送 `main` 或 `develop`。
- 服务端（REST、WebSocket、SQLite 持久化、Dashboard、ML 管线）已实现并通过 21 项 CTest；用户端与管理端当前为模拟数据骨架。各模块真实完成状态以 `docs/backend-todo-temporary.md` 和[工程基础追踪](docs/requirements-traceability.md)为准。

## 许可证

[GNU General Public License v3.0](LICENSE)
