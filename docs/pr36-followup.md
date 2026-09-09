# PR #36 补充修复与文件核对（2026-09-09）

- 安卓系统定位的路线请求显式声明 WGS84，模拟位置声明 GCJ-02；地址搜索不附加坐标类型，交由服务端地理编码。
- 头像上传成功后保存服务端返回的账户版本，确保随后修改昵称使用最新版本。独立契约测试覆盖上传后直接修改昵称。
- 补入 `tools/device_link_sim` 的独立 CMake、帧编解码、说明和测试；严格拒绝非法消息 ID。当前仅完成通信帧核心，连接、并发与 GUI 尚未实现。
- 根目录旧需求副本中新增的 Android 平台描述已合并至 `docs/01-requirements-specification.md`；保留正式文档较新的 UC-U-12 需求。旧追踪副本的模拟器/安卓进展合并至正式追踪文档，未覆盖较新的导航验收限制。
- 根目录两份旧文档保留在本机，不作为第二份需求基线入库。`scripts/dev.sh` 和 `apps/dashboard/vite.config.local.ts` 依赖本机 systemd、桌面环境及开发代理配置，不属于构建依赖，保留为本机联调文件。未包含密钥、数据库或构建产物。

验证结果：独立契约/布局构建与 CTest 3/3、设备模拟器构建与 CTest 1/1、arm64 debug APK 构建、`git diff --check` 与 `NCS_CHECK_BASE_REF=3c7005c ./scripts/check.sh` 均通过。

CI 新增 Qt 6.8.3 独立契约/布局及设备模拟器测试任务。该任务不替代 APK 构建和真机测试。现有 Android 工具链仍提示 AGP 8.6 对 compileSdk 36 的兼容性警告。

历史真机验收不代表此次 GPS 硬件链路已重新验收；本次未重跑手机 GPS 到腾讯地图的现场定位。
