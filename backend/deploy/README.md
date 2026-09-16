# Go 后端部署（B-06）

本目录是 Go 后端的部署与运维交付：进程启动方式、Nginx 站点、密钥注入、迁移门禁、指标抓取、
备份恢复和故障演练。运行前的架构背景见 `docs/backend-architecture.md`，契约见 `api/openapi.yaml`。

## 交付内容

| 路径 | 用途 |
|---|---|
| `systemd/ncs-migrate.service` | 迁移门禁（一次性）：应用 `0001..0012` 并核对版本后退出 |
| `systemd/ncs-{api,worker,outbox-publisher}.service` | 三个常驻进程；均 `Requires=ncs-migrate.service` |
| `systemd/ncs-backend.env.example` | 运行环境变量模板（安装为 `/etc/ncs/backend.env`，`chmod 0600`） |
| `Dockerfile` | 四个可执行文件共用的镜像（`--build-arg NCS_TARGET=cmd/api`） |
| `docker-compose.yml` | 可选的一体化编排（含迁移门禁与 mock-gateway profile） |
| `../scripts/local-stack.sh` | 本地联调：一条命令起全栈（可选带 Nginx） |
| `../scripts/nginx-render.sh` | 渲染 Nginx 模板并校验；`--drill` 在本机跑通一遍 |
| `systemd/ncs-backup*.service|.timer`、`ncs-restore-drill.*`、`ncs-backup-wal.service` | 备份编排：每日全量、周快照、每日校验、每周恢复演练、持续 WAL 归档 |
| `../scripts/backup-postgres.sh` / `restore-postgres.sh` | 备份（自定义格式、本机 7 天 / 周快照 12 周）与恢复 |
| `../scripts/verify-backup.sh` | 备份完整性校验（checksum + `pg_restore --list`，可选真恢复） |
| `../scripts/wal-archive.sh` | WAL 归档（`pg_receivewal`，RPO 的来源） |
| `../scripts/push-backup-remote.sh` | 异地推送（先加密再上传，独立凭据，目的地保留 30 天） |
| `../scripts/drill-backup-restore.sh` | **RPO/RTO 实测演练**（每周；必须实际运行，不能只写在文档里） |
| `../scripts/pitr-drill.sh` | **PITR 重放演练**（每月；隔离临时实例，恢复目标 5～15 分钟） |
| `monitoring/ncs-alerts.yml` | 批准的告警阈值（可加载的 Prometheus 规则） |
| `../scripts/fault-drill.sh` | 依赖故障演练：切断 PostgreSQL/Redis，断言 `/readyz` 翻转与恢复 |
| `../scripts/loadtest-orders.sh` | 并发与重复提交压测（断言一次动作只产生一次效果） |
| `../scripts/verify-closed-loop.sh` | 四进程全链路验证（含 Worker 崩溃恢复、重复投递、Publisher 单活） |
| `../../nginx/ncs-api.conf.template` | Nginx 站点模板（静态、反代、回执来源限制、metrics/readyz 限制） |

## 启动顺序

```text
PostgreSQL / Redis
      ↓
ncs-migrate（一次性）：应用迁移并核对 schema 版本
      ↓
ncs-api（迁移已完成才启动；启动后 migration 仍会幂等地再跑一次）
      ↓
ncs-worker、ncs-outbox-publisher
```

worker 与 publisher **自己不会迁移**：它们启动时会核对 schema 版本，落后就拒绝启动并打印
"run the migration gate first"。这是刻意的——写设备结果的表由迁移引入，带着旧 schema 跑到业务
中途才失败，排查成本远高于启动即失败。

### systemd 安装

```bash
go build -o /usr/local/bin/ncs-api         ./cmd/api
go build -o /usr/local/bin/ncs-worker      ./cmd/worker
go build -o /usr/local/bin/ncs-outbox-publisher ./cmd/outbox-publisher
install -m 0644 backend/deploy/systemd/*.service /etc/systemd/system/
install -m 0640 backend/deploy/systemd/ncs-backend.env.example /etc/ncs/backend.env
chmod 0600 /etc/ncs/backend.env          # 内含设备网关令牌与数据库口令
useradd --system --no-create-home --shell /usr/sbin/nologin ncs
install -d -o ncs -g ncs /var/log/ncs
systemctl daemon-reload
systemctl enable --now ncs-migrate.service ncs-api.service ncs-worker.service ncs-outbox-publisher.service
```

### 容器

```bash
docker build -f backend/deploy/Dockerfile --build-arg NCS_TARGET=cmd/api -t ncs-api:$(git rev-parse --short HEAD) .
# 单进程运行
docker run --rm -e NCS_POSTGRES_DSN=... -e NCS_REDIS_ADDR=... -e NCS_CHARGER_GATEWAY_TOKEN=... ncs-api:dev
# 或者用编排（需要 docker compose 插件）
cd backend/deploy && cp systemd/ncs-backend.env.example .env && docker compose up -d
```

**注意**：验证 B-06 的这台机器上**没有安装 docker compose 插件**（`docker` 可用、`docker-compose`
与 `compose` 子命令不可用），因此 compose 路径只做了结构校验与文档说明，**没有作为验收证据**；
验收以 systemd 与脚本路径为准。使用 compose 前请确认 `docker compose version` 可用。

## 密钥与证书注入（B-06 裁决 ②）

- **证书不入 Git**，也不通过普通环境变量传递内容；只传路径，文件由部署环境挂载：

```text
/etc/ncs/tls/fullchain.pem   0600 root:ncs
/etc/ncs/tls/privkey.pem     0600 root:ncs
NCS_TLS_CERT=/etc/ncs/tls/fullchain.pem
NCS_TLS_KEY=/etc/ncs/tls/privkey.pem
```

- `/etc/ncs/backend.env` 由 systemd `EnvironmentFile` 加载，权限 **0600**、属主 **root:ncs**：

```bash
install -m 0600 -o root -g ncs backend/deploy/systemd/ncs-backend.env.example /etc/ncs/backend.env
```

- **数据库口令、Redis 口令、设备网关令牌只能写入该文件或外部密钥系统**（Vault/KMS/云密钥服务）。
  三者都不得出现在命令行参数、镜像、仓库、CI 日志或 Nginx 配置里。
- `NCS_CHARGER_GATEWAY_TOKEN`：**无默认值**，缺失时 API 拒绝启动；只注入设备网关与 Go API，
  **不得下发给 H5 或普通 Agent**；H5 只能拿用户会话令牌。
- `NCS_PUBLIC_HOST`：真实域名由部署环境注入，**不设默认域名**；模板与代码都不写死。
- 日志中不得输出口令、完整令牌或支付数据；API/Worker/Publisher 的日志只记录 ID 与结果，
  Nginx 访问日志只记录 trace ID 与状态码。
- 本地演练用自签证书（`backend/scripts/nginx-render.sh --drill` 自动生成），仅用于验证配置，不入库。

## 端口与域名（裁决口径）

```text
公网 HTTPS 443（NCS_HTTPS_PORT）→ Nginx → Go API 127.0.0.1:8080（NCS_API_UPSTREAM）
公网 HTTP  80 （NCS_HTTP_PORT）→ 仅 301 跳转到 HTTPS
H5 静态目录 NCS_STATIC_ROOT
/metrics、/readyz：仅内网/本机（Nginx allow/deny + API 只监听本机）
NCS_PUBLIC_HOST：真实域名由部署环境注入，模板与代码都不写死
```

`/metrics` **不登记 OpenAPI**（内部运维端点）；`/readyz` 保留在 OpenAPI 中但属系统运维接口，
H5 与普通 Agent 都不得调用，也不应把它当作业务健康判断。

## 指标端点（B-06 裁决 ④）

三个进程各自暴露 Prometheus 文本格式指标，**只监听本机或内网**，端口可用环境变量覆盖，默认值如下：

| 进程 | 默认地址 | 环境变量 |
|---|---|---|
| Go API | `127.0.0.1:9090` | `NCS_METRICS_ADDR` |
| Worker | `127.0.0.1:9091` | `NCS_METRICS_ADDR` |
| Publisher | `127.0.0.1:9092` | `NCS_METRICS_ADDR` |

填非环回、非私网地址时进程**拒绝启动**（`observability: metrics address must be a loopback or private
address`），除非显式设置 `NCS_METRICS_ALLOW_PUBLIC_BIND=true`。`/metrics` **不登记 OpenAPI**；API 的
业务监听地址默认 `127.0.0.1:8080`，Nginx 侧对 `/metrics` 保留 allow/deny（运维网段之外 403）。

裁决要求的指标与实现系列的对应（**按名字核对，不要凭猜**）：

| 裁决要求的指标 | 实际系列 | 出现时机 |
|---|---|---|
| 消费成功数 | `ncs_worker_events_total{event_type,outcome="succeeded",stream}` | 首次消费成功后 |
| 消费失败数 | `...{outcome="retried"}`（可重试，保留 Pending）、`...{outcome="dead_lettered"}`（进入死信）、`...{outcome="lease_held"}`（租约被他人持有） | 首次出现该类结果后 |
| Pending 数 | `ncs_stream_pending{stream}` | 启动采样后（进程启动即存在） |
| 重试数 | `ncs_worker_retries_total{attempt}` | 首次重试后 |
| 死信数 | `ncs_worker_dead_lettered_total{reason}`（写入）、`ncs_stream_dead_letter_length`（流长度） | 前者首次死信后，后者启动即存在 |
| Stream 延迟 | `ncs_stream_lag{stream}` | 启动采样后 |
| Outbox 未发布数量 | `ncs_pg_outbox_unpublished` | 启动采样后（API 与 Publisher 都暴露） |
| Publisher 失锁次数 | `ncs_publisher_lock_losses_total` | 启动即存在（0） |
| Worker 最近成功时间 | `ncs_worker_last_success_timestamp_seconds` | 启动即存在（0 = 尚未成功过） |
| Publisher 最近成功时间 | `ncs_publisher_last_publish_timestamp_seconds` | 启动即存在（0 = 尚未发布过） |
| PostgreSQL / Redis 连接状态 | `ncs_dependency_up{dependency="postgres"\|"redis"}` | 启动即存在（0，首轮采样后更新） |

补充系列：`ncs_publisher_published_total`、`ncs_publisher_pass_failures_total`、`ncs_publisher_standby`
（1 = 正在等待另一实例释放 advisory lock）、`ncs_api_requests_total{method,route,status}`、
`ncs_api_request_duration_seconds`（histogram）、`ncs_api_requests_in_flight`、`ncs_pg_migrations_version`、
`ncs_redis_capability_failures_total`。

**为什么有些系列一开始不在**：计数器的标签来自流量（事件类型、outcome、attempt、reason），预造这些
标签会造出永远不动的"幽灵 0"，看起来像发生过却从未发生的流量。固定标签的系列（依赖状态、各 stream 的
Pending/Lag/Length、死信长度、未发布数、迁移版本、Publisher 计数与时间戳）在进程启动时就以初值 0 创建，
因此"抓不到"只可能意味着进程真的没跑。

## 指标与告警

`GET /metrics`（Prometheus 文本格式，`text/plain; version=0.0.4`）在 API 进程上暴露：

| 指标 | 含义 | 建议告警 |
|---|---|---|
| `ncs_dependency_up{dependency}` | 依赖是否应答（1/0） | `== 0` 持续 1 分钟 |
| `ncs_pg_migrations_version` | 已应用的最高迁移版本 | `< 7` 立即告警（部署不一致） |
| `ncs_pg_outbox_unpublished` | 未发布 Outbox 行数 | 持续增长（Publisher 停摆） |
| `ncs_api_requests_total{method,route,status}` | 按**路由模板**计数的请求 | 5xx 比例 |
| `ncs_api_request_duration_seconds`（histogram） | 请求耗时 | `histogram_quantile(0.95, ...)` |
| `ncs_api_requests_in_flight` | 正在处理的请求 | 长时间高位 |
| `ncs_worker_*`、`ncs_stream_*` | Worker 与 Streams（Worker 进程日志/metrics） | 死信、Pending 增长 |

### 批准阈值与告警规则（裁决 ③）

批准的初始阈值已落为可加载的规则文件 **`backend/deploy/monitoring/ncs-alerts.yml`**（3 组 12 条）：

| 指标 | 告警条件 | 级别 |
|---|---|---|
| `ncs_dependency_up == 0` | 持续 2 分钟 | Critical |
| `ncs_pg_outbox_unpublished > 100` | 持续 5 分钟 | Warning |
| `ncs_pg_outbox_unpublished > 1000` | 持续 15 分钟 | Critical |
| `ncs_pg_outbox_oldest_unpublished_seconds > 120` | 持续 5 分钟 | Warning |
| `ncs_pg_outbox_oldest_unpublished_seconds > 900` | 持续 10 分钟 | Critical |
| Worker/Publisher 最近成功时间 > 10 分钟 | **存在待处理任务时**，持续 10 分钟 | Warning |
| Worker/Publisher 最近成功时间 > 30 分钟 | **存在待处理任务时**，持续 5 分钟 | Critical |
| `deriv(ncs_stream_pending[10m]) > 0` | 持续 10 分钟 | Warning |
| `ncs_stream_dead_letter_length > 0` | 立即 | Critical |
| `increase(ncs_worker_dead_lettered_total[5m]) > 0` | 立即 | Warning |

"存在待处理任务"在规则里就是 `ncs_pg_outbox_unpublished > 0 or sum(ncs_stream_pending) > 0`：
空库、无事件时 Worker/Publisher 本来就没有"成功"可记，**不这样写就一定会在空闲时误报**。

为支持"Outbox 最老记录超过 N 分钟"这条阈值，B-06 新增了指标
`ncs_pg_outbox_oldest_unpublished_seconds`（最老未发布记录等待秒数，空 Outbox 时为 0），
由 API、Worker、Publisher 的探针一并暴露。

**当前状态**：规则文件的阈值、持续时长与级别与批准表一致，结构可被规则加载器解析。
本机已完成两轮实际验证（记录见 `docs/migration/b-06-golive-verification.md`）：

- 静态与秒级回归：`promtool check rules` 通过；`promtool test rules backend/deploy/monitoring/ncs-alerts.test.yml`
  用合成样本覆盖全部 12 条规则的阈值边界与 `for` 时长，并包含"完全健康（空库）时零告警"的对照；
- 实机触发：真实 Prometheus 加载本文件（sha256 未改动）并以故障值抓取，12 条规则按各自的 `for`
  时长依次转入 firing，且投递链路（Prometheus → Alertmanager → webhook 接收器）收到通知。

上线前仍须在生产监控栈上重跑一次（本机实例与生产拓扑不同），并且**必须补 Alertmanager 配置与真实接收器**：
仓库目前只有规则、没有任何路由/接收器配置，规则 firing 只会停留在 Prometheus 里。

抓取配置示例（Prometheus）：

```yaml
scrape_configs:
  - job_name: ncs-api
    scheme: http
    static_configs:
      - targets: ["10.99.0.11:8080"]     # 内网地址；公网不可达
  - job_name: ncs-worker
    static_configs:
      - targets: ["10.99.0.12:9101"]     # Worker 的指标入口（若已开放）
```

`ncs_pg_outbox_unpublished` 与 `ncs_pg_migrations_version` 只在 PostgreSQL 可达时更新；数据库不可用时
它们保留上次数值，**判断可用性要看 `ncs_dependency_up`**，不要用这两个 gauge 判断存活。

## 备份与恢复（已批准的基线）

| 项目 | 目标 |
|---|---|
| PostgreSQL 备份保留 | 30 天 |
| PostgreSQL RPO | ≤ 15 分钟 |
| PostgreSQL RTO | ≤ 60 分钟 |
| Redis | 可恢复基础设施，不保存唯一业务事实 |
| 账务与订单 | 必须依靠 PostgreSQL 恢复 |

### 编排（裁决 ③：systemd timer，不用 cron）

| 任务 | 单元 | 频率 | 保留 |
|---|---|---|---|
| 全量逻辑备份 | `ncs-backup.service` / `ncs-backup.timer` | 每日 03:20（±10 分钟抖动） | 本机 **7 天** |
| 周备份快照 | `ncs-backup-weekly.service` / `.timer` | 每周日 04:10 | **12 周** |
| 完整性校验 | `ncs-backup-verify.service` / `.timer` | 每日 05:30 | — |
| 恢复演练（实测 RPO/RTO） | `ncs-restore-drill.service` / `.timer` | 每周日 06:00 | 报告随日志保留 |
| WAL 归档（RPO 的来源） | `ncs-backup-wal.service`（常驻，非 timer） | 持续流式 | 由归档目录与磁盘配额决定 |
| 异地推送 | `ncs-backup.service` 的 `ExecStartPost` | 每日，随全量备份 | 对象存储 **30 天** |

```bash
systemctl enable --now ncs-backup.timer ncs-backup-weekly.timer ncs-backup-verify.timer ncs-restore-drill.timer ncs-backup-wal.service
systemctl list-timers 'ncs-*'

# 手工操作
NCS_POSTGRES_DSN=... backend/scripts/backup-postgres.sh              # 每日全量（7 天保留）
NCS_POSTGRES_DSN=... backend/scripts/backup-postgres.sh --weekly     # 周快照（12 周保留）
NCS_BACKUP_DIR=/var/backups/ncs/postgres backend/scripts/verify-backup.sh [--restore]
NCS_POSTGRES_DSN=... backend/scripts/restore-postgres.sh --dump <dump> --dsn postgres://.../ncs_restore
NCS_TEST_PG_DSN=... backend/scripts/drill-backup-restore.sh          # 实测 RPO/RTO
NCS_POSTGRES_DSN=... NCS_BACKUP_WAL_DIR=/var/backups/ncs/wal backend/scripts/wal-archive.sh [--once]
```

### 异地对象存储（裁决 ①：S3 兼容，**配置已批准、尚未实测**）

```text
rclone 远端      ncs-backup-prod          （名称固定）
桶               ncs-prod-backup
前缀与保留       daily/  30 天            weekly/  12 周         （本机 dump 7 天）
加密传输         rclone https/S3（TLS）；不通过明文通道外发
加密存储         上传前 openssl AES-256-CBC/PBKDF2 加密为 .dump.enc；上传时同时请求服务端加密
                 （--s3-server-side-encryption AES256），桶默认加密之外再加一层
版本保护         桶开启对象版本保留：误删或覆盖可回滚
独立凭据         凭据只在 NCS_BACKUP_REMOTE_CONFIG（默认 /etc/ncs/backup-remote.env，0600）与
                 /etc/ncs/rclone/rclone.conf（或外部密钥系统）；与数据库口令、Redis 口令、
                 设备网关令牌完全分离；都不进仓库、都不打印
禁止超级用户     备份任务使用专用角色（见下），不是超级用户
生命周期         桶上配置生命周期规则：daily/ 30 天、weekly/ 12 周过期；脚本里的 rclone delete 只是兜底
```

**当前状态**：本机没有对象存储、没有 rclone，因此**上传路径未实测**，只完成了脚本、前缀、保留与
加密约束的落地与校验（`--dry-run` 可离线检查命令拼装）。生产上线前必须真的推一次并核对 bucket 内容、
版本与生命周期规则。

### 异地备份必须满足（裁决 ① 的要求项）

专用备份角色（最小权限，替代超级用户）：

```sql
-- 逻辑备份读取全部数据；WAL 归档需要 REPLICATION，两者都不需要超级用户。
CREATE ROLE ncs_backup LOGIN PASSWORD '<from the secret store>';
GRANT pg_read_all_data TO ncs_backup;
ALTER ROLE ncs_backup REPLICATION;
-- 连接权限
GRANT CONNECT ON DATABASE ncs_prod TO ncs_backup;
```

`NCS_POSTGRES_DSN` 在备份单元中指向该角色；API/Worker/Publisher 的 DSN 仍用各自的业务角色。

### 演练实例要求（裁决 ④）

```text
NCS_TEST_PG_DSN=<一次性非生产 PostgreSQL 实例 DSN>
  * 不能指向生产库、不能指向开发人员共用库：脚本对库名做检查（ncs_drill_*/*test*/*scratch* 等），
    含 prod/production 一律拒绝；
  * 未配置时**拒绝执行**恢复演练，不回退到任何默认数据库；
  * 账号需具备创建与删除数据库的权限——演练开始前会真的建一个 ncs_drill_capability_check 库并删除，
    权限不足时立刻失败并说明原因，而不是跑到一半才报错；
  * 库名使用 ncs_drill_*，演练结束自动清理，并断言没有 ncs_drill_* 残余库；
  * DSN 不写入仓库（只通过环境或 systemd EnvironmentFile 注入）。
```

当前 WSL 的 `ncs_a03` 账号实测具备 CREATEDB 与 REPLICATION 权限（演练已通过），可继续作为**候选**测试实例；
但生产演练必须使用专门的、可删除的实例，不能默认沿用开发机上的这个库。

### RPO/RTO 的真实来源

```text
RPO ≤ 15 分钟   ncs-backup-wal.service 持续流式归档 WAL（pg_receivewal），因此丢失窗口远小于 15 分钟；
                逻辑备份是每日一次，自身的数据丢失上界是一天，脚本会打印这句话，不冒充 RPO 来源
RTO ≤ 60 分钟   restore-postgres.sh 的实测耗时（drill 报告给出具体秒数）
恢复顺序        WAL 归档 + 最近一次全量备份/dump → 重放 WAL 到目标时间点（PITR）
```

**已实测与未实测（不隐藏）**：dump 备份、校验、恢复与 RPO/RTO 演练已在本机实测（见模块审批单）；
WAL 归档已实测建立复制槽并流式连接（`wal_level=replica`，角色具备 REPLICATION）。**PITR 重放本身未演练**——
本机没有预置第二个 PostgreSQL 实例，因此恢复演练走的是 dump 路径；异地对象存储路径因本机没有对象存储与
rclone，只交付配置与脚本，未作为证据。

## 恢复演练策略（裁决 ②）

| 演练 | 单元 | 频率 | 内容 |
|---|---|---|---|
| 普通 dump 恢复 | `ncs-restore-drill.service` / `.timer` | **每周**（周日 06:00） | `drill-backup-restore.sh`：实测 RPO/RTO，断言备份点前后的数据差异 |
| PITR 重放 | `ncs-pitr-drill.service` / `.timer` | **每月**（第一个周日 07:00） | `pitr-drill.sh`：在隔离临时实例上重放到指定时间点 |

```text
隔离要求       PITR 在**临时 PostgreSQL 实例**上进行（脚本用 pg_basebackup 建数据目录、
               pg_ctl 以独立端口与 socket 目录启动，恢复完成后立即停止并删除），
               绝不在被备份的实例上演练——在被测对象上提升副本会毁掉被测对象
恢复目标       指定时间点前 5～15 分钟（NCS_PITR_REWIND_MINUTES，5..15 校验）
演练记录       恢复目标时间、恢复耗时、归档覆盖范围、失败原因（失败也打印报告，不隐藏）
清理           演练结束停止临时实例、**删除复制槽**（未提供服务的槽会一直占住 WAL 直到磁盘写满）、
               删除临时目录；dump 演练断言不留 ncs_drill_* 库
前置           源实例 wal_level=replica 或 logical、max_wal_senders>=2、角色具备 REPLICATION 与 CREATEDB
```

### PITR 演练实测结果（本机，2026-09-15）

```text
命令：NCS_PITR_SOURCE_DSN=postgres://.../ncs_a03 backend/scripts/pitr-drill.sh
结果：PASS
  postgres tools: /usr/lib/postgresql/16/bin/pg_ctl (PostgreSQL 16.15)
  source ncs_a03: wal_level=replica, max_wal_senders=10
  archive streaming (slot ncs_pitr_drill_<pid>, confirmed by pg_stat_replication)
  base backup taken (34M, mode 700)
  target time: 2026-09-15 09:30:27.431219+00 (between the two markers)
  archived segments: 3
  recovered: before_target=1, after_target=0, latest row at 2026-09-15 09:30:25Z
  result: PASS   recovery duration: 9 seconds

这条结果的含义与边界：
  * 证明的是"WAL 归档 + 基准备份能重建到指定时刻"，且**恰好停在两个已知提交之间**
    （目标点之前的标记存在、之后的标记不存在）——这是 PITR 与"实例起来了"的区别；
  * 演练用的目标点是"该次演练时间线内的一个瞬间"，不是"某个业务事件前 5～15 分钟"；
    要覆盖批准的 5～15 分钟窗口，归档必须已经连续运行了相应时长，脚本会打印归档覆盖范围；
  * 演练在**临时实例**上完成并清理干净（无残留进程、无残留复制槽、无残留目录）。

演练发现并修复的四个真实缺陷（都只在真的跑一遍时才暴露）：
  1. `pg_receivewal --create-slot` **创建槽后立即退出**：原先 WAL 归档服务带着这个参数启动，
     结果是"建槽—退出—（systemd 重启）"的循环，**一字节 WAL 都不会归档**，而 RPO 恰恰依赖它。
     已改为"先单独建槽（已存在则跳过），再以流式模式运行"，并用 `pg_stat_replication` 确认 streaming；
  2. 目标时间用 ISO 的 `T`/`Z` 写法写进 `postgresql.conf` 会被配置解析器拒绝
     （`invalid value for parameter "recovery_target_time"`）→ 改为 `YYYY-MM-DD HH24:MI:SS.US+00`；
  3. `pg_basebackup` 留下的数据目录权限不是 0700 → 启动报 `has invalid permissions`，已 `chmod 700`；
  4. 断言最初连到 `postgres` 库（标记表在源库中），且用 `|| echo missing` 把查询失败伪装成
     "标记不存在" → 改为连接源库，并把查询错误原样报为失败原因。
  另外：`wal-archive.sh --once` 原先会留下一个**没人服务的复制槽**（一直占住 WAL），
  已改为"检查完即删除（若原先不存在）"。

## 故障演练

```bash
NCS_TEST_PG_DSN=... backend/scripts/fault-drill.sh      # 切断 PostgreSQL / Redis，断言 /readyz 翻转与恢复
NCS_TEST_PG_DSN=... backend/scripts/verify-closed-loop.sh   # Worker -9 后 Pending 恢复、重复投递、Publisher 单活
NCS_API_URL=... backend/scripts/loadtest-orders.sh      # 并发与重复提交（同一幂等键只产生一次效果）
```

`fault-drill.sh` 用 TCP 切断（本地转发代理）注入故障，而不是停服务：验证机没有 root，且 PostgreSQL
是共享的。从客户端视角，切断与网络分区等价——正是就绪策略要处理的故障形态。真实部署上用
`systemctl stop postgresql` 跑同样的断言。

## 回滚

- 新增文件（本目录、`nginx/ncs-api.conf.template`、脚本）删除即可回滚，不影响业务代码。
- `cmd/api` 的改动是增量：`/metrics` 路由、就绪探针、`-migrate-only` 开关；回退单个提交即恢复原行为
  （就绪重新由启动时的固定标志决定）。
- Nginx：切回上一份站点文件并 `systemctl reload nginx`。
- 数据库：B-06 不新增迁移，无需数据回滚。
