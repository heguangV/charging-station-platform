# 充电桩设备通信模拟器

这是独立子工程的第一阶段实现，当前完成固定数组帧的编码、严格解码和非法帧测试。它不加入主工程，也不影响 `ncs_server`、`ncs_user` 或 `ncs_admin`。

在具备 Qt 6.2 的环境中，从本目录单独执行 CMake 配置和构建即可。后续按需求继续增加请求调度、重连策略、5 桩并发、桩端和平台端界面；协议字段遵循主需求中的 BootNotification、Heartbeat、StatusNotification、MeterValues 和远程命令约定。
