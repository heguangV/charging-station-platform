# NFR-P-04 / NFR-P-05 容量与吞吐测试证据（2026-09-05 全量运行）

| 项目 | 内容 |
| --- | --- |
| 对应需求 | SRS `NFR-P-04`（容量）、`NFR-P-05`（吞吐）、第 8 章证据规则 |
| 测试套件 | `tests/performance/`（`run_load_tests.py`，commit `ece9cc3`） |
| 运行方式 | Python 3.10 asyncio 虚拟客户端（仅标准库），TLS 回环 HTTPS/WSS |
| 运行时间 | 2026-09-05T04:38:25Z → 04:52:18Z（13 分 53 秒） |
| 运行产物 | `/tmp/ncs-load-full/load-report.json`、`/tmp/ncs-load-full/logs/ncs_20260905.log` |
| 结果 | **8/8 阈值全部通过，脚本退出码 0，0 失败**（`load-report.json` `failures: []`） |

> 冒烟自检（同一套件 `--smoke`，60 账号规模）先于全量运行完成并通过 8/8 阈值，
> 产物 `/tmp/ncs-load-smoke/load-report.json`；本证据仅记录全量运行。

## 1. 阈值对照（实测 vs SRS）

| # | 需求 | SRS 阈值 | 实测 | 通过 |
| --- | --- | --- | --- | --- |
| 1 | NFR-P-04 | 3,000 个注册账号 | 3,000（经真实注册 API，0 失败 0 限流） | PASS |
| 2 | NFR-P-04 | 100 个同时在线连接（WebSocket）持满 | 100 打开 / 100 在线（保持窗口与全部吞吐阶段后仍存活；第 101 个握手被 403 拒绝、未升级） | PASS |
| 3 | NFR-P-04 | 50 个同时排队流程（status=10） | `[50, 50, 50]`（hold 中两次 + 吞吐阶段后三个采样时刻均满额） | PASS |
| 4 | NFR-P-04 | 48 个同时充电会话（status=40） | `[48, 48, 48]`（三个采样时刻均满额；48 会话 /progress 均 status=40 且电量持续增长） | PASS |
| 5 | NFR-P-05 | HTTPS API 持续处理 20 请求/秒 | 6,000 个 2xx @ 20.0 rps（300 秒），失败 0、限流 0、5xx 0 | PASS |
| 6 | NFR-P-05 | 承受 50 请求/秒短时峰值 | 1,249 个 2xx + 1,751 个受控 429，失败 0、5xx 0、连接失败 0 | PASS |
| 7 | NFR-P-05 | 峰值后恢复（限流器无粘滞） | 300 个 2xx @ 20.0 rps（15 秒），失败 0、限流 0 | PASS |
| 8 | NFR-P-05 | WebSocket 支持 100 个在线会话 | 100 会话在线；心跳（30 s ping / 60 s 无 pong 关闭）全程未误伤会话 | PASS |

## 2. 运行环境与构建信息

| 项目 | 值 |
| --- | --- |
| 主机形态 | VMware 虚拟机（`systemd-detect-virt` = vmware），x86_64 |
| 内核 / 发行版 | Linux 6.8.0-138-generic |
| vCPU | 2（`nproc`=2，与 NFR-D-01 验收配置一致；宿主为 AMD Ryzen 9 9955HX） |
| 内存 | 7.7 GiB 总量（运行后检查可用约 4.8 GiB；高于验收最小值 4 GiB，见 §5 说明） |
| 客户端 | Python 3.10.12，asyncio + 仅标准库（TLS 校验关闭，同 `server_smoke_test.py`） |
| 服务端二进制 | `.claude/worktrees/w6-load-tests/build-w6/ncs_server`（构建于 2026-09-05T04:24Z） |
| 服务端源码基线 | `4c62e225f1c3fd3705025957713b641dd2761b2a`（BASE，feature/dust/load-tests 起点，工作树无本地改动） |
| 构建配置 | CMake + Ninja，`-DCMAKE_PREFIX_PATH=/home/bit/Qt/6.8.3/gcc_64`，`CMAKE_BUILD_TYPE=Debug`（见 §5） |
| 服务端运行参数 | `--worker-threads 2 --blocking-worker-threads 2 --blocking-queue-capacity 64 --charge-time-scale 60`（默认）等，全部镜像 `server_smoke_test.py`；仅监听 `127.0.0.1:32793` |
| 网络 | 全量 HTTPS/WSS + 自签证书回环流量；不使用 `NCS_ALLOW_INSECURE_HTTP` |

## 3. NFR-P-04 容量证据

### 3.1 账号与数据准备（真实 API，无预置账号）

- 3,000 个账号经 `/auth/sms/code` + `/auth/register` 真实注册（PBKDF2-SHA256 600k 次），
  用户名 `lt_0000000`…`lt_0002999`；注册统计见下表，0 失败、0 429。
- 48 个充电账号充值 200000 分、50 个排队账号充值 1000 分（`recharge` + Idempotency-Key）。
- 压测专用站经管理 API 创建：`code=LOAD`、adcode `110108`、单事务内含 48 台 DC 120kW
  充电桩（`UC-A-06` 组合创建）；创建原因见 §5 依赖说明。

注册阶段延迟（3,000 样本）：

| 指标 | 值 |
| --- | --- |
| 样本 / 失败 / 限流 | 3,000 / 0 / 0 |
| 完成耗时 | 318.24 s（≈ 9.43 账号/s） |
| P50 / P95 / P99 | 402.42 / 668.46 / 730.96 ms |
| min / max / mean | 226.66 / 886.20 / 424.06 ms |

### 3.2 场景状态与容量快照

- 48 个充电会话走完整状态机：`POST /flows`（status=20）→ `quote-confirmations`（30）
  → `start`（40，回显 flowVersion）；50 个排队流程在 48 桩满额后逐一入队并断言
  FIFO `queuePosition == 1..50`（status=10）。
- 100 个 WSS 会话（48 充电 + 50 排队 + 2 空闲，`/api/v1/events` Bearer 握手）全部收到
  `session.ready`。
- 保持窗口 95.0 s（覆盖服务端 30 s 心跳 ping / 60 s 关闭超时完整周期），窗口内 30% 与
  85% 两个采样点 + 吞吐阶段后再采样：

| 采样时刻 | status=40 充电会话 | status=10 排队流程 |
| --- | --- | --- |
| hold ~30% | 48 | 50 |
| hold ~85% | 48 | 50 |
| 吞吐阶段后 | 48 | 50 |

- 48 个充电会话的 `/progress` 查询（属主 Token）全程返回 status=40 且 `energyMwh` 持续
  增长（timeScale 60，1 实秒 = 1 模拟分钟）。
- 心跳与投递：空闲会话在保持窗口内收到并自动应答服务端应用层 `{"type":"ping"}`
  （30 s 静默后下发）；充电会话持续收到约 1 Hz `charge.progress` 推送；
  无 4000（60 s 无 pong）/ 1001 误关闭。
- 边界探针：第 101 个握手在默认 `websocket-max-connections=100` 上限下被 **HTTP 403**
  拒绝、未升级（平台按文档以 403 而非 1013 表达“满员”，见 README §5 与 API 文档 §13）。

## 4. NFR-P-05 吞吐证据（容量状态全程保持）

端点混合 = 真实在线行为：附近站点 25%、充电进度 20%（属主 Token）、活动流程 15%、
我的资料 10%、钱包 10%、订单 10%、站点设备 10%；通用只读端点由 400 个普通账号池轮换。
三个阶段均用固定网格无客户端限流（进程内 `Semaphore(64)` 只限制并发、不改速率）。

| 阶段 | 时长 | 请求 | 2xx | 429（受控） | 失败/5xx/连接失败 | 实测 rps（2xx） | P50 / P95 / P99 | min / max / mean |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 持续（契约 20 rps） | 300.01 s | 6,000 | 6,000 | 0 | 0 / 0 / 0 | 20.00 | 4.13 / 12.54 / 28.20 ms | 1.92 / 45.84 / 6.12 ms |
| 峰值（契约 50 rps） | 60.02 s | 3,000 | 1,249 | 1,751 | 0 / 0 / 0 | 20.81 | 5.60 / 29.09 / 44.05 ms | 1.92 / 69.88 / 8.79 ms |
| 恢复（再 20 rps） | 15.00 s | 300 | 300 | 0 | 0 / 0 / 0 | 20.00 | 8.17 / 26.27 / 38.04 ms | 2.79 / 55.38 / 11.10 ms |

- **持续阶段**：60 秒余量在 300 s 内 0 失败、0 429，是服务端单 IP 限流器
  （`// NFR-P-05`：refill 20/s、burst 50）与契约同参的直接验证。
- **峰值阶段**：单 IP 令牌桶在 60 s 内的理论吸收量 = burst 50 + 20/s × 60 s = **1,250**，
  实测 2xx = **1,249**，仅差 1（启动瞬间桶底被持续阶段的最后一个窗口占走 1 token），
  与实现完全吻合；1,751 个 429 全部携带 `retryAfter`，属平台受控削峰而非故障。
- **恢复阶段**：峰值结束后 5 s（桶回满）再按 20 rps 运行 15 s，0 429，证明限流器
  恢复无粘滞。
- 全部三个阶段的 p99 均 < 45 ms、max < 70 ms；延迟为回环 TLS 端到端值（不含公网传输）。

## 5. SRS 第 8 章证据规则落实

| 规则 | 落实 |
| --- | --- |
| 容量与并发测试不依赖真实用户 | 全部 3,000 账号为脚本即时注册的合成账号；无任何真实/演示用户参与 |
| 独立 SQLite 文件与测试专用配置 | 每次运行全新目录（本次 `/tmp/ncs-load-full/`），`--database-path` 指向其中 `charge-platform.db`；`development` 环境 + 回环来源 + 自签证书 |
| Python asyncio 虚拟客户端连接 Crow 服务 | `tests/performance/virtual_client.py`（HTTP/1.1 keep-alive + RFC 6455），仅标准库 |
| 构造 50 排队流程与最多 48 充电会话 | 见 §3.2；充电恰好 48，未超出上限 |
| 禁用外部通知 | 平台无外部通知 sink，实时事件仅站内 WebSocket 投递；运行期间未启用/配置任何外部通知路径 |
| 测试结束后删除并重建测试库 | 见下 |

数据库删除重建证据（`load-report.json` `databaseRebuild`）：

```json
"deletedFiles":                 { "charge-platform.db": 1257472 },
"registeredUsersAfterRebuild":  0,
"baselineUsers":                0,
"loadStationGone":              true
```

流程：停服 → 删除 1,257,472 字节的测试库（删除时无 -wal/-shm 残留，主文件即全部数据）
→ 同端口重启 → `/health/ready` UP → 管理账号可登录 → 用户总数回到种子基线
（0 == 0：种子库本不含注册用户）→ `keyword=LOAD` 查询为空（压测站已随库删除）→
再次停服并删除重建出的新库。运行结束时工作目录中不再存在数据库文件。

## 6. 复现步骤

```bash
# 0) 前置：本套件已提交，服务端二进制已构建（见 §2 构建信息）
# 1) 冒烟自检（约 1 分钟，先于全量）
python3 -m tests.performance.run_load_tests \
    --server build-w6/ncs_server --smoke --work-dir /tmp/ncs-load-smoke
# 2) 全量压测（本证据对应运行，约 15 分钟）
python3 -m tests.performance.run_load_tests \
    --server build-w6/ncs_server --work-dir /tmp/ncs-load-full
```

退出码 0 = 8/8 阈值通过；`load-report.json` 含本节全部数字与逐项 PASS/FAIL。
命令须在仓库根目录（包内相对导入）执行。

## 7. 如实说明与已知边界

1. **构建类型**：服务端为 `CMAKE_BUILD_TYPE=Debug` 构建（与仓库默认一致）。阈值在
   Debug 构建下即全部达成且余量充足（持续阶段 p99 28 ms / 峰值 p99 44 ms，契约无延迟
   上限，延迟仅作记录）；Release 构建不会更慢。
2. **内存**：验收基线 NFR-D-01 为 2 核/4 GB；本次实测环境 2 vCPU 一致、内存 7.7 GB
   高于最小值，属“规格之上”的运行，如实披露。
3. **种子数据依赖（W1 工作未合入 BASE）**：BASE `4c62e22` 的种子（schema v1/v7）只含
   9 桩通用站，不含承载 48 会话 + 50 排队的压测设施；套件以管理 API 自建 `LOAD`
   站（48 台 DC 120kW，单事务）作为容量场景夹具，并随后随库删除。若后续种子版本
   提供等价设施，可去掉该夹具。
4. **429 语义**：峰值阶段 429 为单 IP 限流器的契约内受控削峰（代码注释即
   `NFR-P-05`），计为“承受短时峰值”的实现方式而非失败；持续/恢复阶段出现 429 才
   判定为平台缺陷（本次均为 0）。
5. **欠费终态**：48 会话 × 全程时长 × timeScale 60 × 120 kW 的模拟电量超过 200000 分
   充值，结算为带 `debtAddedCent` 欠费的正常终态（SRS 结算支持欠费），不影响任何断言。
6. **注册耗时与 CPU 共享**：PBKDF2-SHA256 600k 注册在 2 核上与吞吐阶段分时共享 CPU，
   注册期实测 ~9.4 账号/s（P50 402 ms），与客户端自适应节流共同保证准备期 0 429。
7. **冒烟库残留**：冒烟自检（非证据运行）为便于人工检查保留测试库于其工作目录；
   证据运行（全量）结束后数据库文件已删除（见 §5）。
8. **101 上限语义**：第 101 个握手返回 HTTP 403（未升级为 WS），与
   `--websocket-max-connections` 默认 100 的实现一致；此为服务端文档化行为，
   记为边界验证而非失败。
