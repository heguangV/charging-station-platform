# Web 用户端本机联调：Vite 代理被服务端 CORS 校验拒绝（未修复）

| 项目 | 内容 |
| --- | --- |
| 发现日期 | 2026-09-14 |
| 状态 | **未修复，已记录**；当前仅用运行期配置规避，未改动任何代码 |
| 影响范围 | `apps/user`（5173）与 `apps/admin`（5174）的 `npm run dev` 本机联调；生产构建（`npm run build` + 静态托管）不受影响 |
| 关联需求 | `UC-U-13`、`NFR-U-01`、`NFR-D-01` |
| 相关文件 | `apps/user/vite.config.js`、`server/middleware/request_policy_middleware.cpp`、`server/runtime/server_config.cpp` |

## 1. 现象

`cd apps/user && npm run dev` 打开页面后，所有接口调用（获取验证码、登录、站点列表、Agent 对话）都返回下面这个结果；`cd apps/admin && npm run dev` 的管理端同样如此（管理员登录也会被拒）：

```json
{
  "success": false,
  "code": 403,
  "message": "origin is not allowed",
  "userMessage": "不允许跨域访问"
}
```

表现为“验证码一直不显示”“附近站点为空”“AI 助手立即报错”，容易被误判为前端缺陷。

## 2. 原因

1. 浏览器向同源的 Vite 开发服务器（`http://127.0.0.1:5173`）发起请求，浏览器按规范自动附加请求头 `Origin: http://127.0.0.1:5173`。
2. `apps/user/vite.config.js`（以及 `apps/admin/vite.config.js`）的 `server.proxy` 把 `/api` 转发给 `ncs_server`。Vite 的 `changeOrigin` 只改写 `Host`，**不改写也不删除 `Origin`**，该头被原样带给服务端。
3. `server/middleware/request_policy_middleware.cpp` 对任何携带 `Origin` 且不在白名单内的请求一律返回 `403`；而 `NCS_CORS_ALLOWED_ORIGINS` 默认是空列表（安全默认：不开放跨域）。
4. 于是“同源”的前端其实是在经代理向服务端发起带 `Origin` 的请求，被自己的安全策略拒绝。

注意这**不是**浏览器 CORS 失败（响应里没有 `Access-Control-Allow-Origin` 问题），而是服务端在应用层主动拒绝。

## 3. 当前规避方式（仅配置，未改代码）

启动 `ncs_server` 时显式放行开发来源：

车主端与管理端的开发端口都要放行（5173 车主端、5174 管理端）：

```bash
NCS_CORS_ALLOWED_ORIGINS=http://127.0.0.1:5173,http://localhost:5173,http://127.0.0.1:5174,http://localhost:5174 \
  ./build/dev/ncs_server --environment development --listen-address 127.0.0.1 \
  --port 8443 --allow-insecure-http true \
  --cors-allowed-origins http://127.0.0.1:5173,http://localhost:5173,http://127.0.0.1:5174,http://localhost:5174
```

对应 `.env` 条目：

```dotenv
NCS_CORS_ALLOWED_ORIGINS=http://127.0.0.1:5173,http://localhost:5173,http://127.0.0.1:5174,http://localhost:5174
```

该项**尚未写入 `.env.example` 与 README 快速开始**，所以新成员按 README 操作仍会踩到该问题。

## 4. 待决的两个修复方案

| 方案 | 做法 | 优点 | 代价 |
| --- | --- | --- | --- |
| A. 只补文档 | 在 `.env.example` 与 README 快速开始写明前端联调必须先配置 `NCS_CORS_ALLOWED_ORIGINS` | 服务端来源校验完全不变，安全模型最保守 | 开发者必须多配一个变量，漏配时的报错（403）不够直观 |
| B. 开发代理去掉 `Origin` | 在 `apps/user`（以及迁移后的其他 Web 前端）`vite.config.js` 的 `/api` 代理里用 `configure` 钩子删除 `Origin` 请求头 | `npm run dev` 零配置可用；语义上也更正确——浏览器↔Vite 那一跳才是同源，`Origin` 对后端无意义 | 在本机开发代理上让服务端的来源校验不生效；需同时在文档说明生产路径不受影响 |

推荐组合：**B + A 的文档说明**。理由：生产环境 Web 用户端是静态托管、直连 API，不经过该代理，服务端的 `NCS_CORS_ALLOWED_ORIGINS` 仍然完整生效；而开发代理是开发者本机进程，删除 `Origin` 不会放宽任何对外暴露面。方案 B 会触及现有安全控制的边界，因此按仓库规则先确认再改。

## 5. 验收与复现

复现（服务端未放行来源时）：

```bash
curl -s -X POST http://127.0.0.1:5173/api/v1/user/auth/sms/code \
  -H 'Origin: http://127.0.0.1:5173' -H 'Content-Type: application/json' \
  -d '{"phone":"13800138000","purpose":"LOGIN"}'
# => {"code":403,...} / 不允许跨域访问
```

放行后同一命令返回 `code:0` 与 `data.developmentCode`。

修复完成后的验收要求：

1. 不设置 `NCS_CORS_ALLOWED_ORIGINS` 时，`apps/user` 与 `apps/admin` 的 `npm run dev` 都能完成登录与主要查询；
2. 带非白名单 `Origin` 的请求仍被服务端拒绝（`401/403`），确认来源校验未被削弱；
3. `apps/user` 的 `npm run test` 与 `npm run build` 通过。
