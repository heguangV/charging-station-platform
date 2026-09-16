# Claude 修复后续验证（2026-09-16）

本轮以 Go API/Go Agent 为目标，保留主工作区 `rebase/postgres-on-develop` 的已有修改。已有修复已按用户要求提交：后端 `1e24942`，前端 `8a648a0`；后续 Agent 超时修复为 `ff672e0`。没有推送、合并或修改远端 PR。远端只读核对结果：develop=`422ee83`、后端 #45=`d6c6dcd`、前端 #44=`d615794`，与交接一致。

## 工作区与修复

- 后端：`/Users/cjy/dsh/csp-worktrees/backend-b08`，`wip/go-hardening-on-b08`。
- 前端：`/Users/cjy/dsh/csp-worktrees/frontend-go-api`，`wip/http-layer-on-go-api-adapter`。
- 保留 Claude 的认证、JSON、地图脱敏、Agent 参数等加固，补回管理接口仍使用的 `encoding/json` 导入，解决编译失败。
- 修复 PostgreSQL 测试数据在同一百分之一秒内截断手机号造成的冲突；订单流程 fixture 用数据库序列分配账号标识，用户检索测试精确限定目标账号。
- Worker 重复投递测试自己运行生产迁移器，并与重建表的仓储测试使用同一 advisory lock，避免空库未初始化或跨包重建表干扰。
- 新增 `TestMigrationFreshAndV9Upgrade`：生产 Go runner 在独立空库执行全部 12 个迁移；0009 带订单、申诉、管理员数据升级到 0012；两条路径重复迁移零新增；验证旧行及新增字段默认值。
- 修复 `verify-closed-loop.sh`：先迁移再导入种子、种子 SQL 出错立即退出、跨平台运行标识、等待 Worker 就绪、隔离指标端口、无请求体取消订单、HTTP `commandId`、按计费时区生成低谷设备时间、按内部事件 `command_id` 精确匹配当前命令并等待消费完成。
- 前端修复轮询达到上限时显示 `commandId`，避免 `undefined`，新增对应回归测试（UC-A-05、NFR-U-01）。
- 更新后端部署文档及 service/compose 注释中落后的迁移版本说明。

没有更改 Idempotency-Key 的 16–128 字符契约、错误码、数据库迁移文件或需求完成状态。后续 Agent 总预算调整见文末。

## 原始基线

从后端 HEAD 导出干净副本到 `/private/tmp/ncs-followup-20260916/baseline`，先运行 `gofmt -l .`、`go vet ./...`、`go test -count=1 -race -json ./...`。

格式与 vet 通过；真实集成测试出现以下上游问题：`TestChargeCommandFailureSemantics` 两个子场景的手机号唯一键冲突，`TestDuplicateDeliveryAppliesTheEventOnce` 的两条消息未确认。后者没有独立迁移准备，也未与仓储重建表测试隔离。应用 Claude 修改后另发现管理接口缺少 JSON 导入。本轮分别修复，未通过关闭集成测试掩盖问题。

## 最终验证

环境：macOS arm64、Go 1.27.1、Node 26、PostgreSQL 18.6、真实 Redis。测试专用库前缀 `ncs_followup_`；Redis 使用本轮独立的回环端口 16389，闭环 DB 14，浏览器 DB 13。没有使用项目现有业务库。

| 验证 | 结果 |
| --- | --- |
| `gofmt -l .` | 无输出 |
| `go vet ./...` / `go build ./...` | 通过 |
| `go test -count=1 -race -json ./...` | 20 个含测试包通过；1305 个测试及子用例通过，零测试跳过；`migrations` 仅为无测试文件包 |
| 生产迁移器：空库、0009 带数据升级、重复迁移 | 通过（包含于 Go 全量） |
| user：`npm run test` / `npm run build` | 122/122，构建通过 |
| admin：`npm run test` / `npm run build` | 158/158，构建通过 |
| dashboard：`npm run test` / `npm run build` | 13/13，类型检查与构建通过 |
| dashboard：`DASHBOARD_TEST_PORT=15300 npm run test:ui` | 10/10 |
| `backend/scripts/verify-closed-loop.sh` | 四个真实进程 + PostgreSQL + Redis，完整通过 |
| 两个 worktree 的 `git diff --check` 与 `bash scripts/check.sh` | 通过；check.sh 无 C/C++ 改动可检查 |

Go 命令设置 `NCS_TEST_PG_DSN`、`NCS_TEST_REDIS_ADDR`、`NCS_REDIS_TEST_ADDR`，因此仓储、短信原子校验、消息重复投递等集成测试真实执行。

闭环验证包括创建/取消订单、启动/停止命令、设备事实回执、重复/冲突回执、冻结账单等待用户确认、重启成功与失败、消息排空、Publisher 互斥、Worker 崩溃重领。真实设备协议、外部支付不在该脚本范围；结算、钱包、退款、评价/申诉由 Go 服务与真实数据库集成测试覆盖。

浏览器实际连接两个 worktree：管理员登录、八个管理页面连续切换、模拟短信用户登录、真实站点列表、钱包读取、未配置模型与地图时 Agent 返回 200/degraded=true 和两个真实站点、设备筛选空数据、网络中断提示与重试恢复。大屏独立 UI 套件验证其现有契约，不代表大屏已接入 Go API。

完整日志：`/private/tmp/ncs-followup-20260916/`，主要文件 `baseline-tests.jsonl`、`final-go-tests.jsonl`、`closed-loop-8.log`、`user-test.log`、`admin-test.log`、`dashboard-ui.log`；构建日志同目录。中间失败日志保留用于归因。

## 仍需明确的边界

- Agent 总超时冲突已在后续修复中解决，慢模型与取消回归通过；真实供应商服务仍需单独验收。
- #44 仍包含从 #42 继承的 C++ 历史代码，本轮没有执行剥离；正式架构/需求矩阵仍存在旧栈描述，未擅自将功能标为完成。
- 真实 LLM、腾讯地图、外部短信/支付、设备 Modbus/OCPP 未配置，未声称通过供应商实网验证。
- 原来的 `node_modules` 软链接是本地依赖，不得加入提交。

## 本轮后续修复（Agent 总预算）

- Agent 从认证入口开始共享 14 秒总预算；规划模型最多使用 4 秒，路由预留 1 秒写响应。
- 已获取的站点、POI 和路线结构化数据在模型或工具超时后保留；响应继续为 200，并带 `degraded=true` 与明确超时/取消提示。
- 调用方取消后不会启动后续工具或模型调用；数据库、地图和模型客户端继续接收并遵守同一 `context`。
- 回归覆盖慢规划、慢模型组织、慢工具、调用方取消，以及真实 HTTP socket deadline；Agent 定向竞态测试通过。
- 该预算不代表真实 LLM、腾讯地图或设备协议实网验收；前端等待上限仍为 20 秒。

### 后续验证结果与记录

- `go vet ./...`、`go build ./...` 通过。
- `go test -count=1 -race -json ./...`：20 个含测试包通过，1311 个测试及子用例通过，零测试跳过；migrations 包无测试文件。使用新建专用 PostgreSQL 库 `ncs_budget_test_0916` 与 Redis 16389。
- 全量之后补充“活动模型收到调用方取消”回归，并运行 `go test -count=1 -race ./internal/agent ./internal/httpapi`，通过。
- 重新运行 `backend/scripts/verify-closed-loop.sh`：通过。使用新建库 `ncs_budget_e2e_test_0916`、Redis DB 12，与此前测试和浏览器进程隔离。
- `git diff --check`、`bash scripts/check.sh` 通过；本轮无 C/C++ 或 Web 代码修改，未重复 Web 构建。
- 后续原始日志位于 `/private/tmp/ncs-budget-20260916/`：`go-tests.jsonl`、`closed-loop.log`。
- 补录上轮独立 C++ 验证：构建成功，CTest 44/44 通过（`/private/tmp/ncs-followup-20260916/legacy-ctest.log`）。macOS Qt 6.11.2 / Apple Clang 不等于正式 Qt 6.2.x / GCC 11+ 基线，未降低仓库工具链要求。
- 只读核对需求矩阵：保留九个业务列，NFR-U-01 在第 261 行仍为“△ 进行中”；没有独立 Agent 条目。未改变矩阵状态，也未将旧栈完成标记移用于 Go 迁移。
