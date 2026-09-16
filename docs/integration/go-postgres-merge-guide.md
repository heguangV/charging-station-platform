# Go / PostgreSQL 后续修复合并说明

> 更新：前端改用干净替代 PR #48。不要继续合入 #44/#47；#48 已包含其 Web 内容与修复。
> 负责人可执行的步骤及 C++ 剔除复现见 [干净前端合并指南](https://github.com/cjyhjy/charging-station-platform/blob/codex/frontend-go-clean/docs/integration/frontend-clean-merge-guide.md)。

## 关联信息

目标：将已经验证的 Go API / Agent、PostgreSQL、Redis 与 Web 适配修复纳入现有交付分支。
关联 NFR-U-01、UC-A-05；继承功能范围和需求映射见现有 PR #45、#44。
本说明描述本次增量合并，不将旧 C++/Qt/SQLite 完成标记转换为 Go 迁移完成证明。

| 增量 PR 来源（cjyhjy fork） | 上游目标分支 | 后续主 PR |
| --- | --- | --- |
| `codex/backend-go-followup` | `codex/backend/b-08-admin-archive-batch` | heguangV/charging-station-platform#45 → develop |
| `codex/frontend-go-clean` | `develop` | heguangV/charging-station-platform#48；替代 #44/#47 |

后端功能修复提交：`1e24942`、`ff672e0`；前端修复提交：`8a648a0`。
验证报告与后续文档提交一起合入。不直接推送 main/develop，不在本次操作中自动合并 PR。

## 变更内容

- 后端认证角色/会话隔离、冻结与注销账号校验、严格 JSON 输入、地图日志脱敏及 Agent 参数校验。
- 修复管理接口编译、数据库测试数据碰撞与 Worker 集成测试迁移隔离；新增生产迁移器空库、0009→0012 保数据升级及重复执行验证。
- Agent 从认证入口共享 14 秒总预算，模型规划最多 4 秒；超时保留已获得数据并返回明确降级提示，取消后不启动后续调用。
- 前端修复 HTTP 请求等待/取消处理和充电桩命令轮询显示 `commandId`。

## 影响与兼容性

运行栈为 `backend/cmd/` 中的 Go API、Worker、Publisher 和开发 Mock Gateway，持久化使用 PostgreSQL，Redis 用于会话及事件流。Web 为 `apps/user`、`apps/admin`。
增量修复不新增数据库迁移，不修改既有错误码或 Idempotency-Key 16–128 字符约束。
完整后端基线需要迁移 0001..0012；单次模型超时配置不能延长 Agent 总预算，前端等待上限仍为 20 秒。
详细配置与进程启动以 [backend README](../../backend/README.md) 和 [部署说明](../../backend/deploy/README.md) 为准；密钥只通过部署环境或不入库的本地配置注入。

## 合并与验证

1. 审查并合并 #46 到 #45 的后端功能分支，确认 CI/审批后将 #45 合入 develop。
2. 更新干净前端 #48 的 develop 基线（如需要），确认 CI/审批与 Go API 联调后将 #48 合入 develop。
3. #48 从 develop 提取 Web 白名单文件，不含 #44 的 C++/CMake/旧后端变更；已含 #47 修复。#48 合并成功后关闭 #44/#47，不再合入或重复 cherry-pick。#42 的其他历史内容单独审查；不删除或改写共享分支。
4. 合并基线使用一次性 PostgreSQL 库及独立 Redis 执行回归；上线前备份数据库，使用生产迁移器执行 0001..0012，不将开发种子导入业务库。
5. 在后端目录执行：

```bash
gofmt -l .
go vet ./...
go build ./...
# 以下三个变量须指向专用测试服务；未设置时部分集成测试会跳过。
NCS_TEST_PG_DSN="$TEST_PG_DSN" \
NCS_TEST_REDIS_ADDR="$TEST_REDIS_ADDR" \
NCS_REDIS_TEST_ADDR="$TEST_REDIS_ADDR" \
go test -count=1 -race ./...
```

6. 在仓库根执行 `bash scripts/check.sh`、`git diff --check`，并运行 `backend/scripts/verify-closed-loop.sh`。该脚本会重置所指测试数据和 Redis 流，必须使用新建的可丢弃测试库、独立 Redis 非零 DB，并按脚本要求设置 `NCS_E2E_ALLOW_DESTRUCTIVE=true`。禁止指向业务库。
7. 分别在 `apps/user`、`apps/admin` 执行 `npm run test`、`npm run build`；用 Go API 验收登录、站点/钱包、充电启停、管理页面与加载/空数据/失败/恢复状态。

已完成证据：Go 全量竞态测试 1311 个测试及子用例通过、20 个测试包通过，零测试跳过；全量之后追加的取消回归和 Agent/HTTP 定向竞态测试通过。构建、vet、四进程真实 PostgreSQL/Redis 闭环通过。前端 user 122、admin 158 个测试与构建通过。
证据范围、环境版本和日志位置详见 [验证报告](codex-followup-verification.md)。本机结果不能替代合并后 CI；本地日志路径不是 GitHub 可下载附件。

## 风险与回滚

本地核心业务闭环已跑通，不等于生产全部验收完成：真实 Modbus/OCPP 设备、外部支付/短信、真实模型/地图供应商仍未实网验收；大屏测试不代表已接入 Go API；旧栈文档与完整需求矩阵仍需完成迁移追踪。

增量修复无数据库变更，应用出现回归时回退相应增量提交或部署到上一已验证应用版本。完整迁移上线若涉及数据库不兼容，先停止写入，按备份恢复流程恢复到匹配的应用/数据库版本；禁止删除已应用迁移记录、直接降级结构或改写共享历史。保留失败日志和测试数据库用于归因。
