# PR #7 dashboard 前端修复记录

关联 UC-W-01～04；审查来源：[PR #7](https://github.com/heguangV/charging-station-platform/pull/7)。本次整合 `upstream/develop` 的 `b47374a`，修复在原个人分支进行，未创建提交或推送。

| 审查项 | 前端处理 | 验收边界 |
| --- | --- | --- |
| 1920×1080 右列裁切与缩放偏移 | 列允许收缩；四指标两行排列；容器显式居中缩放 | 四种分辨率浏览器边界断言通过 |
| 匿名公开降级快照 | 删除合成公开 JSON；禁用 Vite public 发布；只调用受鉴权 summary | 非公开文件导出和受权文件接口待后端提供 |
| 空态与假值 | 移除硬编码 KPI/排行/占比；保留真实零值；区分加载、无数据、错误和过期 | 平均单价、活跃度无契约数据，显示 `--` |
| energyKwh 契约偏差 | 按真实 DTO 使用 energyMwh/predictedEnergyMwh，除以 1,000,000 显示 kWh；日期和计数字段同步对齐 | 前端验证 DTO，不修改后端协议 |
| 目录追踪 | 当前 develop 已修正 SRS 的 apps/dashboard 映射；补充前端状态和本记录 | 未把全部 UC-W 标记完成 |
| pnpm 与 CI | 固定 pnpm 10.15.1，加入独立 dashboard 构建/CTest job | 未降低 Qt/CMake 基线 |
| ECharts 体积 | 按需注册实际图表和组件 | 核心 chunk 约 370 KB；图表等应用 chunk 约 231 KB，构建没有体积警告 |
| 401/403 静默降级 | 清空会话与数据、取消在途请求、停止轮询、显示登录页 | 即时撤销通知和 Crow 页面鉴权仍是后端接入项 |
| Tooltip XSS | 转义站名；校验所有参与计算的数值 DTO | 恶意站名浏览器回归与单位测试通过 |
| 错误提示失真 | 没有快照时明确显示加载失败，有旧数据才提示过期 | 网络恢复回归通过 |

## 验证证据

- `pnpm install --no-frozen-lockfile`：pnpm 10.15.1 安装成功并更新测试依赖锁文件；随后 `CI=true pnpm install --frozen-lockfile` 复验通过。
- `vue-tsc --noEmit && vite build`：通过，无 chunk 超限警告；不发布 `public` 数据目录。
- Vitest：13 项单元/契约回归通过，包括 401/403 停止、空数据/失败/恢复、退出竞态、空闲和绝对会话期限、单位与恶意字段。
- Playwright：7 项浏览器回归通过，覆盖 1920×1080、1366×768、1280×1024、2560×1080 边界；加载失败、真实零值、恢复、403、tooltip 注入和非空后端 DTO。
- `cmake -S apps/dashboard -B /tmp/ncs-dashboard-tests` 与 CTest：独立前端测试工程，`dashboard_contract`、`dashboard_ui` 均通过；本机 Chrome 通过 `DASHBOARD_BROWSER_PATH` 指定，CI 安装 Chromium。
- `git diff --check`：本次修复差异通过。
- `./scripts/check.sh`：本机系统 Bash 3.2 不支持脚本的 `declare -A`，无法完成；未修改仓库脚本。未构建 Qt/Crow，因本次没有修改其代码。

UI 截图由测试生成于 `apps/dashboard/test-results/`（忽略目录）。数据为测试夹具，不是真实运营或后端联调验收证据。

## 保留的接入依赖

详见 [dashboard README](../apps/dashboard/README.md#后端接入依赖与验收边界)。受鉴权文件快照方案已确认，但本次范围仅前端，SRS/API 的文件路径变更随后端接口落地同步；没有增加未实现的路由或宣称 UC-W-02/04 全部完成。快慢充设备数、累计统计口径、服务端页面鉴权与即时撤销通知仍需后端负责人处理。
