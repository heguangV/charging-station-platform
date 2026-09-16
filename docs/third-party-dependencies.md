# 第三方依赖清单

| 依赖 | 基线版本 | 用途 | 许可/获取 | 安全与升级原则 |
| --- | --- | --- | --- | --- |
| Qt | 6.2.x | Core、Widgets、Network、Sql；Charts/WebEngine 按模块启用 | Qt 官方发行；使用前确认团队许可条件 | 小版本升级先做 Windows/Ubuntu 构建与 UI 回归 |
| Qt Multimedia / MultimediaWidgets | 6.2.x（计划） | `UC-U-11` 摄像头枚举、预览和拍照；原 `ncs_user` 目标已随 UC-U-13 移除，改由浏览器媒体设备 API 承担 | Qt 官方发行；Ubuntu 对应 `qt6-multimedia-dev` | 缺少模块时必须降级构建；Web 端改为浏览器权限模型，需回归权限拒绝与无设备状态 |
| Qt WebSockets | 6.2.x（计划） | `UC-X-01` 独立模拟器的 WebSocket 客户端与服务端 | Qt 官方发行；Ubuntu 对应 `qt6-websockets-dev` | 不链接正式 NCS 目标；升级后回归帧限制、断线重连和 5 设备并发 |
| CMake | 3.24+ | 统一构建与安装 | BSD-3-Clause | CI 固定已验证版本，本地不得低于最低版本 |
| Ninja | 1.10+ | 默认构建器 | Apache-2.0 | 跟随受支持系统包 |
| Crow | 待阶段三锁定 | HTTPS REST / WebSocket 服务端 | BSD-3-Clause | 锁定提交/版本并审查传递依赖与安全公告 |
| PostgreSQL | 18.x | 服务端关系数据库、MVCC/WAL、备份恢复 | PostgreSQL License | 使用当前受支持 minor；升级前执行迁移、并发与恢复演练 |
| Qt PostgreSQL / libpq | 与 Qt 6.2+ / PostgreSQL 18 匹配 | QPSQL 数据库驱动与连接协议 | LGPL/GPL / PostgreSQL License | 生产镜像必须包含 QPSQL 插件；启动与集成测试检查驱动可用性 |
| OpenSSL | 受支持系统版本 | HTTPS 与开发证书 | Apache-2.0 | 由系统安全更新维护，禁止旧协议与私钥入库 |
| Python | 3.10+ | ML、测试虚拟客户端和辅助脚本 | PSF | 使用隔离环境和锁定文件；阶段六补齐依赖哈希 |
| Vue 3 / Vue Router / Pinia | 3.4+ / 4.x / 2.x | 三个 Web 前端（`apps/user`、`apps/admin`、`apps/dashboard`）的界面、路由与状态 | 均为 MIT | 锁文件入库，前端构建与测试纳入改动门槛 |
| Vite | 5.x | 三个 Web 前端的开发服务器与静态构建 | MIT | 构建产物不含服务端密钥；升级后回归构建与测试 |
| Vitest / @vue/test-utils / jsdom | 2.x / 2.x / 2x | Web 用户端前端测试 | MIT | 仅开发依赖，不进入运行产物 |
| ECharts | 5.x | 运营大屏图表，以及管理控制台的营收趋势与电桩状态图表 | Apache-2.0 | 两处均按需引入（`echarts/core` + 具体图表组件），锁文件入库；升级后回归容器缩放与 reduced-motion 下不播放动画 |
| Node.js | 22+ | Web 用户端构建与测试（`npm`） | MIT | 不参与 C++ 构建；版本要求在 `apps/user/package.json` 的 `engines` 中声明 |
| 大模型服务（OpenAI-compatible） | 由 `AI_MODEL` 决定 | AI 助手自然语言生成与工具选择；外部 SaaS，非捆绑依赖 | 由所选厂商条款决定，Key 只在服务端 | 未配置时助手必须降级为确定性结果；更换厂商只改配置，不改 Agent 代码 |

新增依赖前记录用途、替代方案、许可证、维护状态、传递依赖、二进制体积、安全记录和升级/移除方案。版本“待锁定”不代表已引入；只有对应源码、锁文件和验证完成后才能标记为已使用。
