# NFR-P-04 / NFR-P-05 容量与吞吐压测套件（tests/performance）

| 项目 | 内容 |
| --- | --- |
| 对应需求 | SRS `NFR-P-04`、`NFR-P-05` 及第 8 章“容量与并发测试证据”段 |
| 运行环境 | Python 3.10+ 标准库（无 pip 依赖）；asyncio 虚拟客户端 |
| 服务端基线 | `server/`（Crow + SQLite），2 核/4GB 验收 VM 推荐配置（`NFR-D-01`） |

## 1. 测试目标（SRS 原文，未重新解释）

- `NFR-P-04`（容量）：支持 3,000 个注册账号、100 个同时在线连接、50 个同时排队
  流程和 48 个同时充电会话。
- `NFR-P-05`（吞吐）：HTTPS API 持续处理 20 请求/秒，并承受 50 请求/秒短时峰值；
  WebSocket 支持 100 个在线会话。
- SRS 第 8 章证据要求：容量与并发测试不得依赖真实用户；测试环境使用独立 SQLite
  文件和仅在测试配置启用的模拟账号，由 Python asyncio HTTP/WebSocket 虚拟客户端
  连接 Crow 服务，并构造 50 个排队流程和最多 48 个充电会话；测试环境禁用外部
  通知；测试结束后删除并重建测试库，不把测试账号或固定密码用于正式演示。

## 2. 文件清单

| 文件 | 职责 |
| --- | --- |
| `virtual_client.py` | 标准库 asyncio HTTP/1.1 + RFC 6455 WebSocket 虚拟客户端、令牌桶节流器、延迟统计 |
| `server_env.py` | 隔离环境管理：空闲端口、自签 TLS 证书、服务端启动参数（镜像 `tests/server_smoke_test.py`）、`/health/ready` 就绪轮询、SIGTERM 停止、测试库删除/重建生命周期 |
| `run_load_tests.py` | 编排器与运行入口：`Runner` 核心（连接池/断言/场景主流程）+ 阈值汇总 + 退出码；把各阶段方法按职责拆到下列模块 |
| `scenario.py` | 全量/冒烟场景规模（3000 账号、48 充电、50 排队、100 在线、吞吐各阶段时长）与共享常量 |
| `provisioning.py` | `ProvisioningMixin`：管理员登录、真实 API 注册 3000 账号、钱包充值、创建压测站 |
| `capacity.py` | `CapacityMixin`：NFR-P-04 六方法——建 48 充电会话、50 排队流程、100 在线会话、容量保持与心跳健康检查 |
| `throughput.py` | `ThroughputMixin`：NFR-P-05 固定网格阶段——20 rps 持续、50 rps 突发（严格）、60 秒过载削峰、恢复 |
| `cleanup.py` | `CleanupMixin`：排队取消、充电结算、WebSocket 关闭、停服→删库→重启重建（SRS 第 8 章） |
| `thresholds.py` | `build_thresholds()`：实测值与 SRS 阈值逐条比对（PASS/FAIL 行） |
| `README.md` | 本说明 |

## 3. 复现步骤

```bash
# 1) 构建服务端（仓库根目录，参考 CI 构建方式）
cmake -G Ninja -B build-w6 \
    -DCMAKE_PREFIX_PATH=<Qt 安装前缀，如 /opt/Qt/6.8.3/gcc_64> \
    -DCMAKE_BUILD_TYPE=Debug
ninja -C build-w6 -j <CPU 核数> ncs_server

# 2) 先跑小规模冒烟（验证脚本自身；约 1 分钟）
python3 -m tests.performance.run_load_tests \
    --server build-w6/ncs_server --smoke --work-dir /tmp/ncs-load-smoke

# 3) 完整压测（约 15-20 分钟，产出 load-report.json 与阈值表）
python3 -m tests.performance.run_load_tests \
    --server build-w6/ncs_server --work-dir /tmp/ncs-load-full
```

退出码 0 = 全部阈值通过。`load-report.json` 含每阶段 P50/P95/P99 延迟、通过率、
容量采样快照与数据库重建证据；把其中的实测数字整理进证据报告
`tests/performance/evidence/`（见 `NFR-P-04-05-load-test-evidence-*.md`）。

> 运行入口为 `python3 -m tests.performance.run_load_tests`（包内相对导入），
> 或把 `tests/` 加入 `PYTHONPATH` 后直接 `python3 tests/performance/run_load_tests.py`。

## 4. 方法与场景构造

### 4.1 环境隔离（SRS 第 8 章 + 研发指南 §6）

- 每次运行使用 `tempfile.mkdtemp`（或 `--work-dir`）全新目录；`--database-path`、
  日志、TLS 证书、模型与仪表盘快照全部落在该目录，绝不触碰开发/演示数据。
- 服务器只监听数字回环地址 `127.0.0.1`，仅 HTTPS/WSS + 自签证书（客户端不校验证书，
  与 `server_smoke_test.py` 相同）；不使用 `NCS_ALLOW_INSECURE_HTTP` 明文例外。
- 端口由 `free_port()` 现取，规避 8080/8443 等可能与陈旧本地二进制冲突的端口。
- 演示账号 `admin/123456` 仅在本次测试数据库中使用（development 配置 + 回环来源）。
- 外部通知：平台无外部通知 sink，实时事件仅站内 WebSocket 投递；证据中如实说明。
- 压测结束删除测试库并重启服务，证明服务端能从零重建干净种子库；随后再次删除。

### 4.2 账号与数据准备

- 3000 个账号全部经真实注册 API（`/auth/sms/code` + `/auth/register`）创建，
  用户名 `lt_0000000`…`lt_0002999`、手机号 `13900000000`…`13900002999`
  （139 号段：13800001001…13800001300 属于 UC-D-02 演示种子的 sim_owner_*
  账号，压测手机号必须避开）；不使用任何预置/演示账号。
- 注册按 PBKDF2-SHA256 600k 次哈希的服务端处理能力自适应节流（自测延迟反馈），
  且所有准备期请求共享 19.5 rps 令牌桶，低于服务端单 IP 限流（20/s、突发 50），
  保证准备阶段零 429。
- 48 个充电账号充值 200000 分、50 个排队账号充值 1000 分（≥500 分流程创建门槛）。

### 4.3 NFR-P-04 容量场景

- 种子站点（9 桩）不足以承载 48 会话 + 50 排队 → 管理 API 一次创建压测专用
  `LOAD` 站（code=`LOAD`，adcode `110108`，使用种子已有的区域基础价 85/50），
  初始即带 48 台 DC 120kW 充电桩（`UC-A-06` 组合创建，单事务）。
- 48 个充电用户依次走真实状态机：`POST /flows`（status=20 报价）→
  `quote-confirmations`（30 预约）→ `start`（40 充电，timeScale 默认 60）。
- 50 个排队用户在全部 48 桩被占满后 `POST /flows`，逐一断言 FIFO
  `queuePosition == 1..50`（status=10）。
- 100 个在线会话：48 充电 + 50 排队 + 2 个空闲账号各建一个 WSS 会话
  （`/api/v1/events`，Bearer Token 握手），全部收到 `session.ready`。
- 容量保持窗口 95 秒（覆盖服务端 30 秒心跳 ping / 60 秒 pong 超时一个完整周期），
  窗口内两次采样、吞吐阶段后再采样一次：管理员查询断言 status=40 恰 48、
  status=10 恰 50；48 个充电会话的 `/progress` 查询全部返回 status=40 且电量增长；
  100 个会话在保持期与吞吐期后全部存活，空闲会话每轮心跳至少应答一次 ping，
  充电会话持续收到 `charge.progress` 推送。
- 边界探针：第 101 个握手在默认 100 上限下必须被 HTTP 403 拒绝、不升级。

### 4.4 NFR-P-05 吞吐场景（容量状态全程保持）

- 端点混合 = 真实在线客户端行为：附近站点 25%、我的资料 10%、钱包 10%、订单 10%、
  活动流程 15%、充电进度 20%、站点设备 10%；通用只读端点由 400 个普通账号池轮换，
  流程端点使用流程属主自己的 Token。
- 持续阶段：固定 20 rps 网格 300 秒（无客户端限流），断言 0 失败、0 429、0 5xx；
  记录 P50/P95/P99 与通过率。服务端单 IP 令牌桶（代码注释即 `NFR-P-05`：refill
  20/s、burst 50）与持续负载同参，是契约实现的直接验证。
- 突发阶段（严格验收）：50 rps 1 秒。令牌桶 burst 50 的直接验证——1 秒内
  50 个请求**全部 2xx**，断言 0 429、0 连接失败、0 5xx；任一 429 即判失败。
- 过载削峰（另立行为测试，**不并入 50 rps 证据**）：50 rps 60 秒持续过载。
  允许的响应仅为 200/201/204 与 429+retryAfter；断言每个请求都被记入
  （2xx + 429 == 总数）、失败 0、5xx 0、429 > 0——429 属平台受控削峰，
  按 2xx/429 分账如实记录。
- 恢复阶段：过载后 5 秒（桶回满）再以 20 rps 跑 15 秒，断言 0 429、0 失败，
  证明限流器恢复无粘滞。

### 4.5 数据库删除重建证据

停服 → 删除 `charge-platform.db`（连同 -wal/-shm）→ 重启 → `/health/ready` UP →
管理账号可登录 → 用户总数回到种子基线 → `keyword=LOAD` 查询为空 → 再次停服并
删除重建出来的测试库。

## 5. 已知边界与如实说明

- 单 IP 限流把 20/s 稳定 + 50 突发写进了服务端（`// NFR-P-05`），因此“50 rps 短时
  峰值”的量化含义 = 1 秒突发内 50 个请求全部成功（令牌桶 burst 50 的直接验证，
  严格验收）；60 秒过载削峰是独立的行为测试——超出部分 429 节流，2xx/429
  分账如实记录，不并入 50 rps 证据。
- 充电 48 会话 × ~15 分钟真实时长 × timeScale 60 ≈ 模拟 15 小时 × 120 kW ≈
  2000+ kWh → 200000 分充值不足以全额抵扣，结算会产生欠费；这是流程的正常终态
  （`SettlementReceipt` 支持 `debtAddedCent`），不影响任何断言。
- 过载阶段 429 属预期受控行为（削峰），不计入“失败”；突发、持续与恢复阶段出现
  429 才是平台缺陷。
- 服务端心跳语义为应用层 `{"type":"ping"}`（30 s）/ 超时关闭（60 s），套件客户端
  自动应答 pong；冒烟模式保持窗口 12 s，不启用心跳断言。
