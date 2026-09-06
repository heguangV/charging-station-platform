# 第三方依赖清单

| 依赖 | 基线版本 | 用途 | 许可/获取 | 安全与升级原则 |
| --- | --- | --- | --- | --- |
| Qt | 6.2.x | Core、Widgets、Network、Sql；Charts/WebEngine 按模块启用 | Qt 官方发行；使用前确认团队许可条件 | 小版本升级先做 Windows/Ubuntu 构建与 UI 回归 |
| Qt Multimedia / MultimediaWidgets | 6.2.x（计划） | `UC-U-11` 摄像头枚举、预览和拍照；仅 `ncs_user` 可选启用 | Qt 官方发行；Ubuntu 对应 `qt6-multimedia-dev` | 缺少模块时必须降级构建；升级后回归无设备、权限拒绝和实机拍摄 |
| Qt WebSockets | 6.2.x（计划） | `UC-X-01` 独立模拟器的 WebSocket 客户端与服务端 | Qt 官方发行；Ubuntu 对应 `qt6-websockets-dev` | 不链接正式 NCS 目标；升级后回归帧限制、断线重连和 5 设备并发 |
| CMake | 3.24+ | 统一构建与安装 | BSD-3-Clause | CI 固定已验证版本，本地不得低于最低版本 |
| Ninja | 1.10+ | 默认构建器 | Apache-2.0 | 跟随受支持系统包 |
| Crow | 待阶段三锁定 | HTTPS REST / WebSocket 服务端 | BSD-3-Clause | 锁定提交/版本并审查传递依赖与安全公告 |
| SQLite | 系统/Qt 驱动对应版本 | 服务端单文件数据库 | Public Domain | 启用约束/WAL，升级前执行迁移与恢复测试 |
| OpenSSL | 受支持系统版本 | HTTPS 与开发证书 | Apache-2.0 | 由系统安全更新维护，禁止旧协议与私钥入库 |
| Python | 3.10+ | ML、测试虚拟客户端和辅助脚本 | PSF | 使用隔离环境和锁定文件；阶段六补齐依赖哈希 |
| Vue / ECharts | 待阶段六锁定 | 运营大屏 | MIT / Apache-2.0 | 锁文件入库，构建前执行依赖审计 |

新增依赖前记录用途、替代方案、许可证、维护状态、传递依赖、二进制体积、安全记录和升级/移除方案。版本“待锁定”不代表已引入；只有对应源码、锁文件和验证完成后才能标记为已使用。
