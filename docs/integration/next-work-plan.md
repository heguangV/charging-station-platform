# 项目下一步工作计划

## 当前交付基线

当前后端基线为 `codex/backend/b-08-admin-archive-batch`，前端基线为
`codex/frontend/go-api-adapter`。后端已经包含 Go API、PostgreSQL、Redis Streams、Worker、Publisher、Mock Gateway、Agent、统计与 B-08 批量管理能力；前端包含用户端和管理端的 Go API 适配。

隔离工作区的联调与加固证据见 [后续验证](codex-followup-verification.md)。
本地修复提交不代表远端 PR 已更新；获授权推送并审查后，再按下列顺序合入共享 `develop`，执行合并基线回归。未合入前不要在新的功能分支上继续扩展共享 OpenAPI 或数据库契约。

旧 `server/`、`core/`、`infrastructure/` 和 Qt/CMake 代码仍是历史栈；当前 Go 运行入口在
`backend/cmd/`。旧 CTest 通过不能替代 Go API 或 Web 验收。#44 含旧栈历史代码，
合并前须核对与 #45 的文件差异，保留 Web 适配提交，不在本次修复中改写共享历史或删除旧栈。

## 合并顺序

1. 合入后端接口与契约收口 PR：先审查 `api/openapi.yaml`、订单查询过滤和 B-08 批量接口。
2. 合入前端 Go API 适配 PR：确认用户端、管理端只通过 `/api/v1` 访问后端。
3. 以合并后的 `develop` 重建一次 WSL 工作区，重新应用迁移 0001..0012。
4. 启动 PostgreSQL、Redis、API、Worker、Publisher、Mock Gateway，再启动用户端 5173 和管理端 5174。

## 全量回归门禁

### 后端

```bash
cd backend
gofmt -l .
go vet ./...
go test -count=1 -race ./...
```

随后使用一次性 PostgreSQL 库执行：迁移、开发种子、登录、站点查询、订单创建、启动/停止命令、设备回执、账单、退款、申诉、统计和 B-08 批量接口。

### 前端

```bash
cd apps/user && npm test -- --run && npm run build
cd ../admin && npm test -- --run && npm run build
```

浏览器验收至少覆盖：登录、站点列表、站点详情、下单、启动/停止、订单小票、评价/申诉、管理端站点/充电桩/用户/申诉/统计页面，以及侧栏连续切换。

## 联调顺序

1. 认证与会话：用户登录、管理员登录、401/403、刷新页面后身份恢复。
2. 只读链路：站点、充电桩、订单、用户和统计列表。
3. 写入链路：站点编辑/启停、充电桩状态、费率、用户冻结/解冻、批量建档。
4. 充电闭环：命令发布 → Mock Gateway 接收 → 设备事实回执 → 订单状态推进 → 账单与桩状态更新。
5. 售后链路：评价、申诉队列、审核退款、审计记录。
6. Agent 链路：降级模式、模型模式、地图服务不可用时的确定性结果。

每一项都记录请求路径、HTTP 状态、响应信封、数据库结果和浏览器页面结果；任一失败先登记为阻塞项，不通过伪造数据绕过。

## 下一阶段待办

- 管理端真实浏览器联调与截图/Network 证据归档。
- 生产部署前的真实对象存储、Prometheus/Alertmanager 和 PITR 演练。
- L4 规模压测：用户检索、站点列表、统计聚合和批量建档；必要时再增加 `pg_trgm` 或聚合索引迁移。
- A-07 后续能力按实际需求立项，不在 P0 闭环上继续引入未登记的接口。

## 回滚原则

前后端 PR 必须保持可独立回滚；数据库迁移只允许追加，不允许修改已经应用的迁移文件。联调失败时优先回滚对应应用提交，保留测试库并导出失败证据，禁止直接删除共享 `develop` 历史。
