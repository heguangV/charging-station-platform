# NCS 运行与运维手册

| 项目 | 内容 |
| --- | --- |
| 适用范围 | 开发、测试和验收环境的启动、检查、备份、恢复与清理 |
| 需求依据 | SRS `NFR-M-04`、`NFR-R-*`、`NFR-D-*`、`UC-U-11`、`UC-X-01` |
| 当前状态 | 服务端正式数据层已切换为 PostgreSQL 18；SQLite 仅保留在测试迁移对照中，不进入 `ncs_server` 正式链接 |
| 更新日期 | 2026-09-15 |

## 1. 环境与配置

要求 PostgreSQL 18（服务端及 client tools）、CMake 3.24+、Ninja、GCC 11+、Qt 6.2.x 及 QPSQL 驱动。复制 `.env.example` 为本机 `.env`，设置文件权限为仅当前用户可读写。`NCS_ENV_FILE` 指定的文件或资产目录 `.env` 提供默认值；进程环境变量覆盖文件，命令行参数优先级最高。

正式环境的服务端必须满足：`NCS_ENVIRONMENT=production`、`NCS_ALLOW_INSECURE_HTTP=false`、`NCS_DATABASE_SSLMODE=verify-full`，并配置可读的 HTTPS 证书/私钥及数据库 CA 根证书。客户端使用 `NCS_ENV=production` 并同样保持 `NCS_ALLOW_INSECURE_HTTP=false`。日志和安全输出不得包含数据库密码、密钥、令牌、验证码或个人信息。

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
| 数据库驱动 | `--database-driver` | `NCS_DATABASE_DRIVER` | `postgresql` |
| PostgreSQL 主机/端口 | `--database-host` / `--database-port` | `NCS_DATABASE_HOST` / `NCS_DATABASE_PORT` | `127.0.0.1` / `5432` |
| PostgreSQL 库/账号 | `--database-name` / `--database-user` | `NCS_DATABASE_NAME` / `NCS_DATABASE_USER` | `ncs` / `ncs` |
| PostgreSQL 密码 | `--database-password` | `NCS_DATABASE_PASSWORD` | 空；推荐只用环境变量或 600 权限 `.env` |
| PostgreSQL TLS | `--database-sslmode` / `--database-ssl-root-cert` | `NCS_DATABASE_SSLMODE` / `NCS_DATABASE_SSL_ROOT_CERT` | `prefer` / 空 |
| 连接超时/上限 | `--database-connect-timeout` / `--database-pool-size` | `NCS_DATABASE_CONNECT_TIMEOUT` / `NCS_DATABASE_POOL_SIZE` | `5` 秒 / `4` |
| 迁移/备份目录 | `--database-migrations` / `--database-backup-directory` | `NCS_DATABASE_MIGRATIONS` / `NCS_DATABASE_BACKUP_DIRECTORY` | `infrastructure/postgres/migrations` / `backups/postgresql` |
| 备份工具 | `--pg-dump` / `--pg-restore` | `NCS_PG_DUMP` / `NCS_PG_RESTORE` | `pg_dump` / `pg_restore` |
| TLS 证书 | `--tls-certificate` | `NCS_TLS_CERTIFICATE` | `secrets/ncs-dev-cert.pem` |
| TLS 私钥 | `--tls-private-key` | `NCS_TLS_PRIVATE_KEY` | `secrets/ncs-dev-key.pem` |
| 本机开发 HTTP | `--allow-insecure-http` | `NCS_ALLOW_INSECURE_HTTP` | `false` |
| Dashboard 快照 | `--dashboard-snapshot` | `NCS_DASHBOARD_SNAPSHOT` | `apps/dashboard/public/data/dashboard.json` |
| Python 解释器 | `--python-executable` | `NCS_PYTHON_EXECUTABLE` | `python3` |
| ML 工作脚本 | `--ml-worker-script` | `NCS_ML_WORKER_SCRIPT` | `ml/worker.py` |
| ML 活动模型 | `--ml-model-path` | `NCS_ML_MODEL_PATH` | `ml/models/load_rf.pkl` |
| 腾讯地图服务端 Key | `--tencent-map-key` | `NCS_TENCENT_MAP_KEY` | 空（地图能力降级） |
| AI 提供方 | `--ai-provider` | `NCS_AI_PROVIDER` | `openai`（另支持 `deepseek`、`qwen`、`claude`、`custom`） |
| AI 模型 | `--ai-model` | `NCS_AI_MODEL` | 空（AI 助手降级为确定性结果） |
| AI 端点 | `--ai-base-url` | `NCS_AI_BASE_URL` | 空（按 provider 取默认 HTTPS 端点） |
| AI 密钥 | `--ai-api-key` | `NCS_AI_API_KEY` | 空（不回显、不记日志、不返回响应） |
| AI 超时 | `--ai-timeout-ms` | `NCS_AI_TIMEOUT_MS` | `15000`（允许 1000～60000） |

`--environment` 允许 `development`、`test`、`acceptance`、`production`。开发模式会启用演示凭据，因此只允许监听数字回环地址；通配地址、多播地址和非法地址会在启动前被拒绝。未显式配置的文件路径以服务程序所在部署目录为稳定基准（源码构建会自动定位项目资源），不随 shell 当前目录漂移。

环境文件：`NCS_ENV_FILE` 指定的文件（必须存在且可读）或资产目录下的 `.env`（存在时加载）提供可由进程环境变量覆盖的默认值；条目名与上表环境变量一致，例外是腾讯地图服务端 Key 写作 `TENCENT_MAP_SERVER_KEY`，AI 配置写作 `AI_PROVIDER`、`AI_MODEL`、`AI_BASE_URL`、`AI_API_KEY`、`AI_TIMEOUT_MS`。Web 用户端在开发/构建时另读取 `TENCENT_MAP_JS_KEY` 和受限来源 `TENCENT_MAP_JS_ORIGIN`，不会保留 Server Key 或 AI Key。AI 助手未配置 `AI_MODEL`/`AI_API_KEY` 时自动退化为确定性的工具检索结果，不阻断服务启动。`.env` 为 Git 忽略的仅本机文件（权限 600），真实 Key 不得提交；可运行 `./scripts/configure-local-map.sh` 安全写入并以 `--check` 验证，详见 `docs/tencent-map-setup.md`。

### 1.2 新增增强任务依赖

`UC-U-11` 原计划使用 Qt Multimedia 和 MultimediaWidgets；用户端改为 Vue 3 Web 客户端后（`UC-U-13`），摄像头采集改由浏览器媒体设备 API 与浏览器权限模型承担，`ncs_user` Qt 目标已移除。

`UC-X-01` 计划使用 Qt WebSockets，Ubuntu 开发环境需提供 `qt6-websockets-dev`。模拟器使用独立 CMake，依赖缺失不得影响根工程或正式可执行目标。实现后单独构建方式为：

```bash
cmake -S tools/device_link_sim -B build/device-link-sim -G Ninja
cmake --build build/device-link-sim
ctest --test-dir build/device-link-sim --output-on-failure
```

上述模拟器命令只在 `tools/device_link_sim/CMakeLists.txt` 实现后可用；规划期不得将命令输出作为构建证据。

### 1.3 首个 OWNER 引导（一次性离线引导）

生产库初始没有任何非演示 OWNER，须先引导一个才能使用管理端（UC-A-09 扩展流）。`--bootstrap-owner` 是一次性离线启动模式，不监听网络；仅当库中不存在 `is_demo=0` 且角色为 OWNER 的管理员时生效：

```bash
export NCS_ADMIN_BOOTSTRAP_KEY='<10-128 位一次性初始密码>'
ncs_server --database-host 127.0.0.1 --database-name ncs --database-user ncs \
  --bootstrap-owner <账号名>
```

- 初始密码取自环境变量 `NCS_ADMIN_BOOTSTRAP_KEY`（10-128 位），不回显、不写日志；用毕应立即注销该变量。
- 账号名须为 3-32 位 ASCII 字母、数字或下划线。
- 创建的账号：角色 OWNER、非演示账号、状态启用、`must_change_password=1`——首次登录管理端会提示修改密码，改密成功后旧密码立即失效。
- 账号创建与 `ADMIN_CREATED` 审计在同一事务提交；任一校验失败（密钥缺失或长度不符、账号名非法、用户名已存在、已存在非演示 OWNER）时退出码为 1 且数据库不变。
- 引导成功后正常启动 `ncs_server`，用该账号登录即可。

## 2. 构建与启动顺序

```bash
export QT_CMAKE=/path/to/Qt/6.2.x/gcc_64/bin/qt-cmake
./scripts/configure.sh dev
./scripts/build.sh dev
./scripts/test.sh dev
./scripts/smoke-test.sh
```

完整运行时按以下顺序启动：

1. 启动 PostgreSQL，确认业务账号只能访问目标数据库且生产 TLS 验证成功。
2. 启动 `ncs_server`，等待 `/api/v1/system/health/ready` 成功（校验 PostgreSQL schema、读写能力、WAL 能力和迁移版本，任一失败返回 503）。
3. 在浏览器打开 Web 车主端与管理端（`npm run dev` 或托管 `dist/`），确认连接状态正常。
4. 需要时启动 Dashboard 与 ML；两者失败不得阻断基础充电结算。

阶段一可直接执行：

```bash
./scripts/smoke-test.sh                          # 临时 PostgreSQL + 服务端回环冒烟
cd apps/user  && npm run test && npm run build   # 车主端前端冒烟
cd apps/admin && npm run test && npm run build   # 管理端前端冒烟
```

### 2.1 SQLite v9 存量数据切换

`ncs_sqlite_to_postgres` 是一次性迁移工具，仅接受 checksum 为 `ncs-v9-order-review` 的 NCS SQLite v9 文件。目标 PostgreSQL 必须是尚未运行 NCS 初始化的空数据库；任何已有用户表、视图或序列都会导致拒绝，包括仅有演示账号的库。迁移期间不得启动服务或让其他工具修改目标库。

切换顺序：停止 SQLite 旧服务的全部写入 → 保留原文件及 `-wal`/`-shm` 快照 → 新建空 PostgreSQL 数据库 → 执行下列迁移 → 核对逐表行数与业务抽查 → 切换 `ncs_server` 连接配置。工具持有 SQLite 只读事务快照，并在一个 PostgreSQL 事务内取得与服务初始化相同的 advisory lock、检查空库、建表、搬运、核对源/目标行数并同步 identity；失败连同新建 schema 一起回滚为空库。搬迁不运行完整演示数据生成器，仅清除当前事务内 v1/v6 SQL 创建的引导行；`schema_version` 保留 PostgreSQL 自身的迁移标记。

```bash
export NCS_DATABASE_PASSWORD='<目标库密码>'
./build/dev/infrastructure/postgres/ncs_sqlite_to_postgres \
  --sqlite /absolute/path/to/charge_platform.db \
  --database-host db.example.internal --database-port 5432 \
  --database-name ncs --database-user ncs_migrator \
  --database-sslmode verify-full \
  --database-ssl-root-cert /absolute/path/to/postgres-ca.pem \
  --confirm-fresh-target
unset NCS_DATABASE_PASSWORD
```

SQLite 时期的 `backup_record` 审计行会保留，但其文件路径会转换为 `legacy-sqlite://unavailable`、验证状态转为 `LEGACY_SQLITE`，防止被误当成 `pg_restore` 归档。切换后立即产生并隔离恢复验证一份新的 PostgreSQL 备份；确认回退窗口结束前不删除 SQLite 快照。

## 3. 运行检查

| 检查项 | 正常结果 | 异常处置 |
| --- | --- | --- |
| 进程存活 | `/health/live` 返回成功 | 检查进程退出码与脱敏应用日志 |
| 服务就绪 | `/health/ready` 返回成功 | 检查配置、数据库版本、锁和外部依赖状态 |
| 客户端连接 | 状态栏显示服务可用 | 校验主机、端口、证书信任和系统时间 |
| 日志 | `ncs_YYYYMMDD.log` 持续写入 | 检查目录存在、空间和写权限 |
| 数据库连接 | 业务账号仅有目标 schema 所需权限，生产为 `verify-full` | 收紧角色/网络规则，轮换泄露凭据并复查审计 |

应用日志保留 30 天，审计/运维日志保留 180 天。清理任务必须按记录类别执行，不得用应用日志清理逻辑删除审计证据。

## 4. 备份与恢复（阶段二/三）

- 日常逻辑备份使用 `pg_dump --format=custom`；生产另配置 `pg_basebackup` 与 WAL 归档，禁止在线复制 PostgreSQL data directory。
- 每日快照保留 7 份、每周快照保留 4 份；备份记录写入 `backup_record`。
- 快速验证先校验文件大小、SHA-256 与 `pg_restore --list`；完整恢复验证必须创建隔离数据库并实际执行 `pg_restore`，不得覆盖运行库。
- 每次验证记录备份编号、校验结果、schema 版本、耗时和操作者；目标为 RPO 24 小时、RTO 4 小时。

恢复流程：停止写入并保留现场 → 选择最近已验证备份/WAL 恢复点 → 创建隔离数据库 → `pg_restore` 或时间点恢复 → 运行 schema、外键及业务抽查 → 切换数据库连接配置 → 启动就绪检查 → 记录恢复结论。任何一步失败都恢复原连接配置，不删除现场数据。

## 5. 故障处理

| 场景 | 第一响应 | 禁止事项 |
| --- | --- | --- |
| 数据库连接/认证失败 | 停止写请求，核对脱敏连接状态、角色、`pg_hba.conf` 与证书 | 输出密码/连接串，反复重建数据库 |
| schema 版本不匹配 | 保持只读/未就绪，核对迁移清单 | 修改已执行迁移 |
| 锁超时或死锁 | 记录请求 ID，检查 `pg_stat_activity`、锁顺序和长事务 | 在事件循环中重试 SQL，自动重放含外部副作用的事务 |
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

脚本拒绝处理 production。PostgreSQL 测试库只能以明确库名由专用清理命令删除并重建，禁止对未解析变量或默认库执行 `dropdb`；执行前确认备份、日志和测试证据已归档。开发证书使用 `./scripts/generate-dev-cert.sh` 生成，仅限回环或受控 VM，30 天后替换。

## 7. 运维记录

每次部署、备份验证、恢复、证书替换或数据清理至少记录：时间、环境、操作者、变更版本、请求/任务编号、执行命令、结果、异常、回滚和证据位置。记录中只写相对或受控标识，不写密钥与个人信息。
