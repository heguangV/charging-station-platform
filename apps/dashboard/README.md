# NCS 运营大屏

Vue 3、TypeScript、Pinia 与 ECharts 前端，正式目录为 `apps/dashboard/`。需求见 [SRS UC-W-01～04](../../docs/01-requirements-specification.md)，接口见 [API §10](../../docs/database-api.md)。

## 开发与验证

使用 Node.js 22.12+ 和 `package.json` 固定的 pnpm 10.15.1。浏览器需支持 `AbortSignal.timeout/any`（Chrome/Edge 124+）。

```bash
cd apps/dashboard
pnpm install --frozen-lockfile
pnpm dev
pnpm run build
pnpm test
pnpm exec playwright install chromium
pnpm run test:ui
```

也可将真实测试注册到独立 CTest 工程，无需 Qt：

```bash
cmake -S apps/dashboard -B build/dashboard-tests
ctest --test-dir build/dashboard-tests --output-on-failure
```

开发入口为 `http://localhost:3000`，`/api` 代理到本机 `http://127.0.0.1:8080`。生产构建输出 `dist/`；由部署方配置 HTTPS 同源 `/api/v1/dashboard/*` 反向代理和 Crow 受权页面入口。Vite 开发代理不进入生产构建。

## 已实现的前端行为

- 1920×1080 等比居中缩放，三列按剩余空间分配，四张指标卡采用两行排列。
- 登录调用 `/auth/login`，退出调用 `/auth/logout`；Token 仅存本页内存，刷新页面需重新登录。
- 登录后每 30 秒读取 `/summary`；401/403 清空数据、取消在途请求、停止轮询并显示登录页。8 小时最长会话、30 分钟本页无操作退出，轮询不会延长前端空闲期限。
- 网络失败后仅保留当前登录已经成功读取的内存数据，明确标记过期并继续重试。没有数据时显示失败或空态，不生成演示值。恢复后清除告警。
- 按当前服务端 DTO 接收整数分、整数毫瓦时和 UTC 秒，仅展示时换算；预测按电站选择，避免把不同电站接成同一条曲线。
- ECharts 按需注册；站名等文字经 HTML 转义，数值字段先作 DTO 校验。
- `publicDir: false`，不提供或复制任何 `public/data/dashboard.json`，即使运行时后端重新生成该文件也不会被 Vite 发布。

## 后端接入依赖与验收边界

本次只修复前端，以下项目没有标记完成：

- **UC-W-02 受鉴权文件快照**：已确认应由后端在非公开目录保存并通过鉴权接口读取。当前后端只有 `/summary`，前端不调用不存在的 `/snapshot`，不回退匿名文件。现有 `/summary` 返回 `stale` 时可展示过期快照；整个服务不可达时只能保留本页内存数据。后端负责人需落实接口及导出路径，再同步 SRS/API。
- **UC-W-01 设备占比**：后端 `chargerTypeShare.fast/slow` 实际统计订单次数，不能冒充设备数量。因此对应面板显示“暂无设备类型统计”，待设备统计契约明确后接入。
- **统计时间范围**：当前 `totalChargeCount`、`totalRevenueCent`、站点排行实际使用近 30 日订单，前端如实标注近 30 日；累计指标需后端提供累计口径后恢复。时段数据是 UTC 周几/小时的电量聚合，展示近 30 日时段电量，不虚构百分比负荷率。
- **UC-W-04 服务端入口与撤销通知**：前端登录页不能替代 Crow 页面鉴权。账号撤销最迟在下一次请求的 401/403 被识别；服务端页面保护、即时撤销通知及真实多角色端到端验收由后端协作完成。

回归使用测试目录的明确合成夹具，不会进入生产包；测试结果见 [PR7 修复记录](../../docs/dashboard-pr7-fixes.md)。
