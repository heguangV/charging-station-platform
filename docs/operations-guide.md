# NCS 运行与运维手册

| 项目 | 内容 |
| --- | --- |
| 适用范围 | 开发、测试和验收环境的启动、检查、备份、恢复与清理 |
| 需求依据 | SRS `NFR-M-04`、`NFR-R-*`、`NFR-D-*`、`UC-U-11`、`UC-X-01` |
| 当前状态 | 服务端（REST/WebSocket/SQLite/Dashboard/ML 管线）已交付；管理端与大屏前端尚未实现；新增拍照头像与设备模拟器命令待对应产物实现后启用 |
| 更新日期 | 2026-09-05 |

## 1. 环境与配置

要求 CMake 3.24+、Ninja、GCC 11+ 和 Qt 6.2.x。复制 `.env.example` 为本机 `.env`，设置文件权限为仅当前用户可读写。配置文件依次由命令行 `--config`、`NCS_ENV_FILE`、当前目录 `.env`、程序目录 `.env` 选择；进程环境变量再覆盖文件中的同名配置。

正式环境的服务端必须满足：`NCS_ENVIRONMENT=production`、`NCS_ALLOW_INSECURE_HTTP=false`，并配置可读的证书和私钥路径。客户端使用 `NCS_ENV=production` 并同样保持 `NCS_ALLOW_INSECURE_HTTP=false`。日志和安全输出不得包含配置值中的密钥、令牌、验证码或个人信息。

本机开发联调可在两个进程中显式设置 `NCS_ALLOW_INSECURE_HTTP=true`。服务端还必须使用 `NCS_ENVIRONMENT=development` 和 `NCS_LISTEN_ADDRESS=127.0.0.1`（或 `::1`）；客户端必须使用 `NCS_ENV=development` 和相同的数字回环地址。服务端启动日志必须出现明文开发模式 WARNING；证书文件在此模式下不读取。完成联调后取消该变量即可恢复默认 HTTPS。

### 1.1 服务端启动配置

命令行参数优先于同名进程环境变量，进程环境变量优先于环境文件；均未设置时使用受控的本机开发默认值。执行 `ncs_server --help` 查看完整参数。

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

`--environment` 允许 `development`、`test`、`acceptance`、`production`。开发模式会启用演示凭据，因此只允许监听数字回环地址；通配地址、多播地址和非法地址会在启动前被拒绝。未显式配置的文件路径以服务程序所在部署目录为稳定基准（源码构建会自动定位项目资源），不随 shell 当前目录漂移。

环境文件：`NCS_ENV_FILE` 指定的文件（必须存在且可读）或资产目录下的 `.env`（存在时加载）提供可由进程环境变量覆盖的默认值；条目名与上表环境变量一致，仅腾讯地图服务端 Key 写作 `TENCENT_MAP_SERVER_KEY`。用户端地图另读取 `TENCENT_MAP_JS_KEY` 和受限来源 `TENCENT_MAP_JS_ORIGIN`，不会保留 Server Key。`.env` 为 Git 忽略的仅本机文件（权限 600），真实 Key 不得提交；可运行 `./scripts/configure-local-map.sh` 安全写入并以 `--check` 验证，详见 `docs/tencent-map-setup.md`。

### 1.2 新增增强任务依赖

`UC-U-11` 计划使用 Qt Multimedia 和 MultimediaWidgets，Ubuntu 开发环境需提供 `qt6-multimedia-dev`。依赖未安装时，根 CMake 和 `ncs_user` 必须成功配置与构建，仅不编译摄像头适配和拍照对话框。

`UC-X-01` 计划使用 Qt WebSockets，Ubuntu 开发环境需提供 `qt6-websockets-dev`。模拟器使用独立 CMake，依赖缺失不得影响根工程或正式可执行目标。实现后单独构建方式为：

```bash
cmake -S tools/device_link_sim -B build/device-link-sim -G Ninja
cmake --build build/device-link-sim
ctest --test-dir build/device-link-sim --output-on-failure
```

上述模拟器命令只在 `tools/device_link_sim/CMakeLists.txt` 实现后可用；规划期不得将命令输出作为构建证据。

## 2. 构建与启动顺序

```bash
export QT_CMAKE=/path/to/Qt/6.2.x/gcc_64/bin/qt-cmake
./scripts/configure.sh dev
./scripts/build.sh dev
./scripts/test.sh dev
./scripts/smoke-test.sh
```

完整运行时按以下顺序启动：

1. 启动 `ncs_server`，等待 `/api/v1/health/ready` 成功（校验 SQLite schema、读写能力、WAL 和迁移版本，任一失败返回 503）。
2. 启动 `ncs_user` 和 `ncs_admin`，确认连接状态正常。
3. 需要时启动 Dashboard 与 ML；两者失败不得阻断基础充电结算。

阶段一可直接执行：

```bash
./build/dev/server/ncs_server --smoke-test
QT_QPA_PLATFORM=offscreen ./build/dev/apps/user/ncs_user --smoke-test
QT_QPA_PLATFORM=offscreen ./build/dev/apps/admin/ncs_admin --smoke-test
```

## 3. 运行检查

| 检查项 | 正常结果 | 异常处置 |
| --- | --- | --- |
| 进程存活 | `/health/live` 返回成功 | 检查进程退出码与脱敏应用日志 |
| 服务就绪 | `/health/ready` 返回成功 | 检查配置、数据库版本、锁和外部依赖状态 |
| 客户端连接 | 状态栏显示服务可用 | 校验主机、端口、证书信任和系统时间 |
| 日志 | `ncs_YYYYMMDD.log` 持续写入 | 检查目录存在、空间和写权限 |
| 数据目录 | 仅服务端账号可读写 | 立即收紧权限并排查客户端越界访问 |

应用日志保留 30 天，审计/运维日志保留 180 天。清理任务必须按记录类别执行，不得用应用日志清理逻辑删除审计证据。

## 4. 备份与恢复（阶段二/三）

- 备份只能经 SQLite Online Backup API 或设计文档允许的一致性机制生成，运行时禁止直接复制数据库文件。
- 每日快照保留 7 份、每周快照保留 4 份；备份记录写入 `backup_record`。
- 恢复验证必须在隔离目录和独立服务进程中完成，不覆盖正在运行的数据库。
- 每次验证记录备份编号、校验结果、schema 版本、耗时和操作者；目标为 RPO 24 小时、RTO 4 小时。

恢复流程：停止写入并保留现场 → 选择最近已验证备份 → 在隔离目录恢复并校验 → 切换服务端数据库路径 → 启动就绪检查 → 抽查活动流程、钱包和审计记录 → 记录恢复结论。任何一步失败都回到原数据库路径，不删除现场文件。

## 5. 故障处理

| 场景 | 第一响应 | 禁止事项 |
| --- | --- | --- |
| 数据库无法打开 | 停止写请求，保留错误与文件权限证据 | 反复重建或覆盖原库 |
| schema 版本不匹配 | 保持只读/未就绪，核对迁移清单 | 修改已执行迁移 |
| 锁超时 | 记录请求 ID，检查长事务与工作线程 | 在事件循环中重试 SQL |
| 结算中断 | 按幂等键和恢复检查点重放 | 人工直接改余额 |
| 证书失效 | 切换已审核证书并重启验证；本机开发可按第 1 节启用受限回环 HTTP | 临时开放局域网或公网明文 HTTP |
| 外部地图/ML 失败 | 地图先确认 Server Key、配额和出口限制；路线失败时保留浏览器导航及本地距离，ML 启用朴素预测降级 | 阻断基础充电流程；把 Server Key 下发客户端 |
| 摄像头不可用 | 确认 Qt Multimedia 模块、操作系统摄像头权限、设备占用状态和视频输入枚举结果；继续提供本地选图 | 忽略摄像头错误、循环重启设备或阻塞 UI |
| 设备模拟器频繁重连 | 确认平台是否监听、检查封顶退避记录、设备 ID 重复和回环端口占用 | 关闭退避、开启无上限循环或将未鉴权 `ws://` 监听到非回环地址 |

## 6. 数据清理

测试或开发运行数据只能显式执行：

```bash
./scripts/clean-runtime-data.sh test --yes
./scripts/clean-runtime-data.sh development --yes
```

脚本拒绝处理 production。清理不可从 Git 恢复；执行前确认数据库、日志和测试证据已按需要归档。开发证书使用 `./scripts/generate-dev-cert.sh` 生成，仅限回环或受控 VM，30 天后替换。

## 7. 运维记录

每次部署、备份验证、恢复、证书替换或数据清理至少记录：时间、环境、操作者、变更版本、请求/任务编号、执行命令、结果、异常、回滚和证据位置。记录中只写相对或受控标识，不写密钥与个人信息。
