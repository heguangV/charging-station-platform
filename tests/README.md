# NCS 测试约定

测试按研发指南分为 `unit`、`database`、`contract`、`integration`、`ui`、`e2e` 和 `nonfunctional`。新增测试时在 `tests/CMakeLists.txt` 注册真实可执行测试，并用 CTest `LABELS` 标出层级与对应 SRS 编号；不得用空目标表示测试已覆盖。

当前阶段一包含：

- `ncs_foundation_tests`：公共错误码、配置、日志与敏感字段过滤；
- `ncs_server_smoke`：服务端配置和日志初始化；
- `ncs_user_smoke`、`ncs_admin_smoke`：无桌面环境窗口启动。

数据库、日志、截图、模型和临时配置必须使用测试专属目录。测试不得读取或清理开发、演示和正式数据，也不得依赖真实账号、真实短信、真实地图 Key 或外部通知。
