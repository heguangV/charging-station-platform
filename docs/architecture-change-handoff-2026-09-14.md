# 架构改造交接说明（2026-09-14）

| 项目 | 内容 |
| --- | --- |
| 用途 | 跨对话交接：本轮改造做了什么、仓库现在什么状态、还有哪些待决事项 |
| 仓库 | `/Users/cjy/dsh/charging-station-platform` |
| 分支 / HEAD | `main`（跟踪 `origin/main`），HEAD = `5e147e1` |
| **提交状态** | **本轮没有做任何 commit / push / PR**；全部改动留在工作区 |
| 关联需求 | `UC-U-13`、`UC-U-14`、`UC-A-*`、`BR-13`、`BR-14`、`NFR-U-02`~`NFR-U-05`、`NFR-M-05`、`NFR-S-06`、`NFR-S-07` |

---

## 1. 本轮完成的四件事

### 1.1 车主端 Qt Widgets → Vue 3 + HTML5 响应式 Web（`UC-U-13`）

`apps/user` 从 Qt C++ 客户端改为 Vue 3 + Vite 单页应用：同一套页面同时适配 PC（侧边栏）与手机（底部导航），断点 900/901px 全部由 CSS 控制，无第二套页面、无 UA 嗅探。浏览器定位用标准 Geolocation API，腾讯地图 JavaScript API 直接在页面内渲染（不再经过 Qt WebView）。原 Qt 源码 39 个文件已删除，`apps/mobile` 按要求保留为迁移参考（已冻结）。

### 1.2 新增 AI 出行助手一级模块（`UC-U-14`）

- `agent/`：`AgentService` + `AgentTool` 抽象 + 四个工具 `station_search` / `station_detail` / `poi_search` / `route`。编排链：LLM 工具选择 → 依赖补齐（route/detail 自动补站点锚点）→ 顺序执行 → 观察文本回灌 LLM → 自然语言 + 结构化结果。
- `infrastructure/ai/`：OpenAI-compatible 客户端与配置，provider 可换（openai / deepseek / qwen / claude / custom），端点强制 HTTPS。
- `infrastructure/map/`：`tencent_map_client`（统一 WebService 出口）、`poi_service`、`route_service`。
- `server/controller/agent_controller.*`：`POST /api/v1/user/agent/chat`，只做 HTTP/鉴权/DTO/校验。
- 依赖方向固定 `server → agent → core / infrastructure`；Agent 只读、不碰 SQLite、不改任何业务状态。
- **大模型未配置时自动降级**为确定性工具结果 + 固定文案（`llmUsed=false`、`degraded=true`），不阻断对话。

### 1.3 管理端 Qt Widgets → Vue 3 + HTML5 Web 控制台

`apps/admin` 从 2795 行 Qt C++ 改为 Vue 3 + Vite + ECharts 控制台，9 个页面覆盖原 Qt 版全部功能，并补齐后端早已支持但 Qt 版没做界面的两块（管理员账号、审计与备份运维）：登录/重新认证、总览（营收趋势 + 每日明细 + 电桩状态 + 健康度）、站点与基础价格版本、充电桩与远程重启、用户与订单历史、活动流程强制释放、智能预测与 ML 任务、管理员账号、运维。敏感操作统一走 `auth.runWithReauth`（命中 `REAUTH_REQUIRED` → 弹窗 → **同一幂等键**重试一次）。

### 1.4 两端统一设计语言与动效（`NFR-U-04`、`NFR-U-05`）

`apps/user` 与 `apps/admin` 的 `src/styles/motion.css` **逐字节相同**，设计令牌（`--ncs-brand/surface/line/muted/radius/shadow/r-*/s-*/dur-*/ease-*`）一一对应：毛玻璃顶栏与侧栏、发丝描边 + 三级叠加柔和投影、内联 SVG 线性图标（emoji 已清零）、等宽数字。

四类动效两端同构：**加载**骨架屏（承载容器保留真实 `data-testid`）、**切换** `<Transition name="page" mode="out-in">` + 导航选中指示条生长、**数据更新** `useValueFlash` 只切 class 的高亮（首渲染不闪、**不改文本/数值**）、**划动** `v-reveal` 滚动进入 + `scroll-behavior: smooth` + 列表错开。降级唯一收口在 `motion.css` 的 `prefers-reduced-motion` 与 `useReducedMotion.js`。

---

## 2. 工作区状态（重要）

```text
35 个已修改（M）  70 个已删除（D）  34 个未跟踪（??）
暂存区：仅 3 项删除（tests/admin_api_smoke_test.py、admin_http_fixture.h、admin_ui_contract_test.cpp）
        —— 由 git rm --cached 产生，其余改动均未暂存
```

**没有任何 commit / push / PR / 分支操作。** 提交前请先复核暂存区（`git diff --cached`），并按 `docs/development-guide.md` §7.2 排除 `node_modules`、`dist`、`.env`、`screenshots/`。

**新增顶层内容**：`agent/`、`apps/user/src|tests|public`、`apps/admin/src|tests|public`、`infrastructure/ai/`、`infrastructure/map/` 三个新文件对、`core/application/{llm_client.h,poi_service.h}`、`server/controller/{agent_controller.*,station_dto.h}`、`tests/agent_*_test.cpp`、`tests/llm_client_test.cpp`、`docs/web-client-dev-cors.md`。

**两个 `package-lock.json` 目前未被 Git 跟踪**，按 dashboard 提交 pnpm-lock 的惯例应随应用一起提交。

---

## 3. 验证证据

| 检查 | 结果 |
| --- | --- |
| C++ 全量构建（Qt 6.11.2 / CMake 4.4.3 / Ninja / AppleClang 21，`NCS_ENABLE_STRICT_WARNINGS=ON`） | **零错误**。删掉 Qt 管理端后，原先挡路的 `apps/admin` `-Woverloaded-virtual` + `-Werror` 问题随之消失 |
| CTest | **36/37 通过**。唯一失败 `ncs_sqlite_repository`（备份保留断言 `seven daily and four weekly copies`）**已在 HEAD 的隔离 worktree 中复现，确认是既有问题**，与本轮改动无关 |
| `apps/user` 前端 | `npm run test` **89 项通过**（11 文件）+ `npm run build` 通过 |
| `apps/admin` 前端 | `npm run test` **136 项通过**（13 文件）+ `npm run build` 通过 |
| 密钥泄漏 | 注入假 Key 构建后确认：车主端只注入 `TENCENT_MAP_JS_KEY`；管理端产物中无任何 Key |
| 分层边界 | `core`/`infrastructure` 无 `agent` 引用；`agent` 无 `server` 与 sqlite 引用 |
| 真实浏览器验收 | 车主端与管理端均对着真实 `ncs_server` 跑通：登录、附近 5 站点、AI 结构化结果、管理端 9 页 0 报错、390×844 抽屉导航 |
| `git diff --check` | 干净 |
| 截图 | `screenshots/preview/before/`（改版前 4 张）、`screenshots/preview/after/`（改版后 13 张，含管理端 9 页）——该目录已被 gitignore |

**本机无法执行的验证**：`./scripts/check.sh` 用了 `declare -A`，macOS 自带 bash 3.2 不支持（无 bash 4+）。已按其逻辑手工执行等价门禁：`git diff --check HEAD -- '*.cpp' '*.h'` 干净、无文件超 700 行、变更的 C/C++ 文件 `clang-format --dry-run --Werror` 全部通过（本机无 clang-format，临时在 `/tmp` 装 venv 验证，未污染仓库）。

---

## 4. 待决事项（下一轮需要用户拍板）

### 4.1 Web 前端本机联调 CORS（已记录未修复）

`npm run dev` 开箱即用会全线 **403 不允许跨域访问**：浏览器带 `Origin: http://127.0.0.1:5173|5174`，Vite 代理原样转发，而服务端 `NCS_CORS_ALLOWED_ORIGINS` 默认为空。完整记录见 [`web-client-dev-cors.md`](web-client-dev-cors.md)。两个方案待选：

- **A. 只补文档**：在 `.env.example` 与 README 写明联调必须先配置该变量。服务端来源校验完全不变。
- **B. 开发代理去掉 `Origin`**：`npm run dev` 零配置可用；生产环境前端直连 API、不经过代理，来源白名单仍生效。

当前只是运行期用 `--cors-allowed-origins` 规避，**代码与 `.env.example` 都没改**。

### 4.2 数据库从 SQLite 换 PostgreSQL（仅做了评估，未动代码）

已完成代码调查，结论：**分层预留是干净的，但没有"改配置就能换"的预留**。

- 已有：9 个仓储端口在 `core/application`（约 118 个虚方法），SQLite 只是其中一个实现，且已有 `InMemory*` 实现证明端口可替换；`core` 里零 SQL；`withTransaction` 抽象了事务边界；ML 走内部 HTTP、前端只走 REST，都不碰库；备份方法声明在 `AdminRepository` 端口上。
- 缺口：方言 SQL 硬编码（`INSERT OR` 12、`AUTOINCREMENT` 9、`strftime` 9、`ON CONFLICT` 6、`last_insert_rowid` 5）；v1..v9 迁移 SQL 内嵌在 `sqlite_repository.cpp`（4145 行）；**并发契约完全不同**——现状是"每连接单线程 + `BEGIN IMMEDIATE` + `busy_timeout` + WAL"的单写者模型，PG 的 MVCC 没有 `BEGIN IMMEDIATE` 的对应物，需要重新论证所有写路径的隔离正确性；备份/恢复是文件级语义（Online Backup API + 快照 + 页级校验 + 损坏测试）；`main.cpp` 直接 `new SqliteRepository`，无后端工厂与连接配置。
- 另外：SRS 技术栈表、§2.1、`BR-09`、`NFR-S-03`、`UC-D-01` 与 `database-design.md` 都写死了 SQLite，**换库本身是一次需求变更**。

若要推进，建议顺序：ADR-005 + 改 SRS/拆 `database-design.md` → 引入 `RepositoryFactory` + `NCS_DATABASE_DRIVER`（SQLite 保持默认）→ 迁移文件双方言化 → 实现 PG 适配器并让两后端跑同一套契约测试 → 单独设计写事务隔离语义 → 备份/恢复按驱动分派 → sqlite→pg 数据迁移与切换演练。

### 4.3 其他开放项

- **`GET /admin/ml-tasks`（历史任务列表）在接口文档与服务端都不存在**：只有 `POST /admin/ml-tasks` 与 `GET /admin/ml-tasks/{taskNo}`。管理端预测页因此做成"触发 + 按任务号轮询"并在页面注明，没有臆造接口。若需要历史列表，要先加服务端接口（新需求）。
- **`apps/dashboard` 大屏本轮未改**（确认的范围是车主端 + 管理端）。要统一设计语言可以再做。
- **`station_dto.h` 有一份重复**：`station_routes.cpp` 仍保留等价的局部 `stationJson`。该文件在 HEAD 已有 269 处历史格式债，整文件 clang-format 会产生与本轮无关的大量改写，故当时撤销了对该文件的修改，头文件里留了 TODO。
- **`tests/server_http_smoke_test.py` 已停用**：它驱动旧的 Qt 车主端二进制做 TLS 联调，目标不存在了。文件里加了停用说明，建议后续改为 Node/浏览器侧的 Web 冒烟。服务端侧仍有 `ncs_server_smoke` 与 `ncs_admin_routes` 等覆盖。
- **`NFR-P-06`（AI 助手响应预算）无压测证据**；真实大模型与真实腾讯 Key 下的端到端验收待配置凭据。

---

## 5. 本机环境差异（换机器/换对话要注意）

| 项 | 本机情况 |
| --- | --- |
| Qt | Homebrew `qtbase` **6.11.2**（≥ 仓库最低 6.2，但非仓库基准版本） |
| 编译器 | AppleClang 21（**不是 GCC 11+**）；`apps/admin` 已移除，不再有 `-Woverloaded-virtual` 阻塞 |
| CMake / Ninja | 4.4.3 / 1.13.2 |
| bash | 3.2（`scripts/check.sh` 无法运行，见 §3） |
| clang-format | 未安装（当时用 `/tmp/ncs-fmt-venv` 临时验证） |
| 凭据 | **未配置** `TENCENT_MAP_SERVER_KEY` / `AI_API_KEY`，所以 POI、真实路线与大模型均走降级路径 |

---

## 6. 本地运行时（当前已启动）

| 服务 | 地址 | 启动方式 |
| --- | --- | --- |
| `ncs_server` | http://127.0.0.1:8443 | `nohup` + `disown` 脱离任务（pid 61212），数据库 `/tmp/ncs-preview/preview.db`，日志 `/tmp/ncs-preview/` |
| 车主端 dev | http://127.0.0.1:5173 | `apps/user` 内 `npm run dev`（受管后台任务，可能被回收） |
| 管理端 dev | http://127.0.0.1:5174 | `apps/admin` 内 `npm run dev`（同上） |

登录：车主端任意 1 开头 11 位手机号，验证码直接显示在页面上；管理端 `admin` / `123456`。

**重启服务端会让所有登录失效**——按 `UC-D-01`，会话与验证码是内存服务，刻意不落库，进程重启即失效。这是设计如此，不是缺陷；要持久化会话需先改 SRS。

启动命令（含联调所需来源放行）：

```bash
# 服务端（development + 回环 HTTP，仅本机联调）
nohup env NCS_ENVIRONMENT=development NCS_ENV=development \
  NCS_LISTEN_ADDRESS=127.0.0.1 NCS_SERVER_PORT=8443 NCS_ALLOW_INSECURE_HTTP=true \
  NCS_DATABASE_PATH=/tmp/ncs-preview/preview.db NCS_LOG_DIR=/tmp/ncs-preview/logs \
  ./build/dev/ncs_server --environment development --listen-address 127.0.0.1 --port 8443 \
  --allow-insecure-http true --database-path /tmp/ncs-preview/preview.db \
  --log-directory /tmp/ncs-preview/logs \
  --cors-allowed-origins http://127.0.0.1:5173,http://localhost:5173,http://127.0.0.1:5174,http://localhost:5174 \
  > /tmp/ncs-preview/server.out 2>&1 & disown

# 前端
cd apps/user  && VITE_NCS_API_TARGET=http://127.0.0.1:8443 npm run dev -- --host 127.0.0.1 --port 5173 --strictPort
cd apps/admin && VITE_NCS_API_TARGET=http://127.0.0.1:8443 npm run dev -- --host 127.0.0.1 --port 5174 --strictPort
```

---

## 7. 新对话可以直接用这句话开场

> 读 `docs/architecture-change-handoff-2026-09-14.md` 了解当前状态。我要先处理第 4 节里的待决事项（CORS 修复方案 / PostgreSQL 迁移评估），或者按第 3 节的方式重新跑一遍验证。

## 8. 关键文件索引

| 主题 | 文件 |
| --- | --- |
| 车主端 | `apps/user/src/{style.css,styles/motion.css,views/,components/,api/,stores/,services/}`、`apps/user/tests/` |
| 管理端 | `apps/admin/src/{style.css,styles/motion.css,views/,components/,api/,stores/,charts.js,utils/}`、`apps/admin/tests/` |
| Agent | `agent/include/agent/`、`agent/src/{agent_service.cpp,tools/,prompts/}` |
| AI / 地图基础设施 | `infrastructure/ai/llm_client.*`、`infrastructure/map/{tencent_map_client,poi_service,route_service}.*` |
| Agent 接口契约 | `docs/database-api.md` §5A |
| 需求基线变更 | `docs/01-requirements-specification.md`（`UC-U-13`、`UC-U-14`、`UC-A-02`、`NFR-U-02`~`U-05`、`BR-13`、`BR-14`） |
| 架构与阶段 | `docs/development-guide.md` |
| 状态与证据 | `docs/requirements-traceability.md`、`docs/risk-register.md`（新增 R-13~R-16） |
| 已知 CORS 问题 | `docs/web-client-dev-cors.md` |
