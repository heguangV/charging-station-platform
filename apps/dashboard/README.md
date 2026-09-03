# NCS 可视化运营大屏 (apps/dashboard)

本项目为 **NCS（新能源充电桩综合管理平台）** 的 Web 可视化大屏模块，基于 **Vue 3 + TypeScript + Vite + Pinia + ECharts 5** 构建。

---

## 一、 满足需求矩阵（UC-W-01 ~ UC-W-04）

- **UC-W-01 大屏布局**：
  - 1920×1080 设计基准，基于视口比例实现动态等比自适应缩放（`scale` 算法），适配大屏及各种显示设备。
  - **顶部**：平台名称、毫秒级实时走字时钟、在线/离线状态指示徽标、数据截止时间、全屏控制。
  - **左列**：电桩实时状态分布（空闲绿、充电橙、故障红、维护蓝）饼图；各电站充电量排行 TOP 5 横向柱状图。
  - **中列**：4 大核心指标卡（累计充电次数、累计总营收元、在线桩数、注册用户数）；近 30 日营收与充电量趋势双轴面积图。
  - **右列**：24 小时充电负荷时段分布；快充 vs 慢充类型占比环形图；未来 24 小时 AI 负荷预测与高峰预警双曲线。
- **UC-W-02 双模数据源与容灾降级**：
  - 每 30 秒自动轮询服务端受权接口 `GET /api/v1/dashboard/summary`（依据 `docs/database-api.md §10.1` 契约）。
  - 当后端不可用或网络异常时，平滑降级读取本地离线快照 `public/data/dashboard.json`，并触发顶部告警横幅，杜绝大屏崩溃或留白。
- **UC-W-03 深度图表交互**：
  - 具备 Tooltip 悬浮详情、图例筛选联动、一键全屏切换（F11 / 快捷按钮）及手动强制刷新。
- **UC-W-04 访问安全与只读设计**：
  - 纯只读数据可视化，支持请求头携带鉴权 Token，防止非授权越权改动。

---

## 二、 目录结构

```text
apps/dashboard/
├── public/
│   └── data/
│       └── dashboard.json     # 离线容灾降级数据快照 (UC-W-02)
├── src/
│   ├── api/
│   │   └── client.ts          # API 客户端与容灾回退逻辑
│   ├── assets/                # 静态资源
│   ├── components/            # 大屏可视化组件群
│   │   ├── HeaderBar.vue           # 顶部栏与时钟
│   │   ├── MetricCards.vue         # 核心 KPI 4 卡片
│   │   ├── ChargerStatusChart.vue  # 电桩状态占比图
│   │   ├── StationRankChart.vue    # 站点排行柱状图
│   │   ├── RevenueTrendChart.vue   # 30 日走势折线图
│   │   ├── HourlyHeatmapChart.vue  # 24 小时负荷分布
│   │   ├── ChargerTypeChart.vue    # 快慢充环形图
│   │   ├── LoadPredictionChart.vue # 负荷预测与高峰预警
│   │   └── OfflineAlert.vue        # 降级模式警告横幅
│   ├── stores/
│   │   └── dashboardStore.ts  # Pinia 状态管理与 30s 轮询调度
│   ├── types/
│   │   └── dashboard.ts       # 契约数据模型定义
│   ├── App.vue                # 大屏网格布局与等比自适应容器
│   ├── main.ts                # 入口文件
│   └── style.css              # 深色科技风全局主题
├── index.html
├── package.json
├── tsconfig.json
└── vite.config.ts
```

---

## 三、 本地开发与构建

### 1. 安装依赖
推荐使用 `pnpm`：
```bash
cd apps/dashboard
pnpm install
```
*(亦可使用 `npm install`)*

### 2. 本地开发运行
```bash
pnpm dev
```
启动后访问：`http://localhost:3000`

### 3. 类型检查与生产打包
```bash
pnpm run build
```
打包产物将输出至 `apps/dashboard/dist/` 目录。
