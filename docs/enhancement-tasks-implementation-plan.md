# 新增增强任务实施路径

| 项目 | 内容 |
| --- | --- |
| 适用需求 | SRS `UC-U-11`、`UC-X-01` |
| 用途 | 定义模块边界、实施顺序、交付物和验证门禁 |
| 当前状态 | 两项均未开始实现；头像服务端能力可复用 |
| 更新日期 | 2026-09-05 |

本文只定义实现方式，可观察行为和验收结果以 SRS 为准。目录和文件名为规划交付物，在源码和验证证据存在前不得标记完成。

## 1. 共同实施原则

- `UC-U-11` 是正式用户端 `UC-U-05` 的增量能力，必须复用现有 REST 头像契约，不得增加客户端 SQLite 或本地业务真相。
- `UC-X-01` 是独立子工程，规划路径为 `tools/device_link_sim/`，根 CMake 不调用其 `add_subdirectory`，正式目标不链接其代码。
- 两项任务均使用 Qt 信号/槽和异步网络接口，不在 GUI 线程中进行文件编码、阻塞等待或循环重试。
- 新的编译宏、依赖和包含目录限定在具体目标，不使用全局 `add_compile_definitions`。
- 实现按“可单测的纯逻辑 → 传输/设备适配 → Controller → GUI → 端到端”递进，每个阶段有独立退出条件。

## 2. UC-U-11 拍照上传头像

### 2.1 现有基础与缺口

| 能力 | 当前状态 | 实施处理 |
| --- | --- | --- |
| 服务端上传、真实格式/尺寸校验、重编码 | 已实现并有契约测试 | 直接复用，不增加新头像接口 |
| `UserApi::uploadAvatar` | 已声明，界面未调用 | 改为可接收文件或内存载荷的统一上传入口 |
| 受权头像读取 | 服务端已实现，客户端只支持 JSON 包装 | 增加二进制响应类型和 `ETag` 处理 |
| 个人中心头像 | 仍读取演示服务的本地路径 | 改为登录后从服务端加载，上传成功后刷新 |
| Qt Multimedia | 未引入 | 作为 `ncs_user` 的可选、目标级依赖 |

### 2.2 模块分解

| 模块 | 规划文件 | 职责 | 依赖 | 验证 |
| --- | --- | --- | --- | --- |
| AV-01 二进制网络适配 | `apps/user/net/api_client.*`、`user_api.*` | 上传内存图片；读取头像字节、`Content-Type`、`ETag`；保持 Bearer 鉴权 | 现有 `ApiClient` | multipart 请求和二进制响应单测 |
| AV-02 图片处理 | `apps/user/avatar/avatar_image_processor.*` | 解码、居中正方形裁剪、缩放、旋转方向归一、压缩至 200 KiB 内 | Qt Gui | 横图/竖图裁剪、不可压缩输入和大图测试 |
| AV-03 摄像头适配 | `apps/user/avatar/camera_capture_source.*` | 列举设备、启停摄像头、异步捕获 `QImage`、统一错误 | Qt Multimedia | 伪适配器单测；真实设备人工验收 |
| AV-04 拍照对话框 | `apps/user/ui/avatar_capture_dialog.*` | `Loading/Preview/Frozen/Processing/Error` 状态，预览、拍摄、重拍、取消 | AV-02、AV-03、Qt MultimediaWidgets | 无设备、被占用、重复点击和取消 UI 测试 |
| AV-05 个人中心编排 | `user_main_window_profile.cpp`、`user_main_window_state.cpp`、`user_main_window.h` | 选择图片来源，发起上传，显示加载/失败/恢复，登录后加载真实头像 | AV-01～04 | 真实 REST 端到端和重启加载 |
| AV-06 可选构建 | 根与 `apps/user/CMakeLists.txt` | 只在目标存在时加入摄像头源文件、链接库和 `NCS_HAS_CAMERA=1` | AV-03～04 | 有/无 Multimedia 两套配置构建 |
| AV-07 文档与验收 | `tests/`、运维手册、需求追踪 | 自动化证据、Ubuntu/Windows 实机记录和降级截图 | AV-01～06 | SRS `UC-U-11` 全部验收项 |

### 2.3 实施路径

1. **A0：基线冻结**
   - 以 `POST /api/v1/user/me/avatar`、`GET /api/v1/user/me/avatar/content` 和资料 `version` 为契约，不改 schema、公开错误码或服务端存储方式。
   - 退出条件：契约测试继续通过，客户端不保存服务端路径。
2. **A1：先完成 UC-U-05 真实头像闭环**
   - 实现 AV-01，把现有本地选图改为真实上传，再从受权 URL 刷新。
   - 退出条件：上传成功、失败保留原头像、重启后可重新加载。
3. **A2：交付纯图片管线**
   - 实现 AV-02，使本地选图和拍照共用同一输出结构。
   - 退出条件：不同宽高和大小输入稳定产生正方形、不超过 200 KiB 的载荷。
4. **A3：接入摄像头与对话框**
   - 实现 AV-03～04，通过注入式适配器使大多数 UI 状态不依赖真实硬件测试。
   - 退出条件：预览、拍摄、重拍、取消和硬件错误的状态转移可验证。
5. **A4：集成与降级**
   - 实现 AV-05～06；未找到 Multimedia 时不编译 AV-03～04。
   - 退出条件：两套 CMake 配置通过，本地选图回归通过。
6. **A5：系统验收**
   - 完成 AV-07，收集有摄像头、无摄像头和无 Multimedia 三类证据。

## 3. UC-X-01 充电桩设备通信模拟器

### 3.1 独立工程边界

规划结构如下，所有路径在实现前均为计划：

```text
tools/device_link_sim/
├── CMakeLists.txt
├── README.md
├── docs/protocol.md
├── core/
├── pile_app/
├── platform_app/
└── tests/
```

- 子工程使用独立 `project()`、CTest 和严格警告，目标为 `device_link_core`、`pile_simulator`、`platform_simulator` 及真实测试可执行项。
- 必需依赖仅为 Qt 6.2.x `Core`、`Network`、`WebSockets`、`Widgets`；测试目标增加 `Test`。
- 默认使用回环 `ws://127.0.0.1`完成教学联调。若未来允许非回环连接，必须另行增加 TLS、设备鉴权、配置和安全验收。
- 与主工程联动不在首次交付范围。未来联动必须先修改 SRS、接口契约和幂等/故障处理测试。

### 3.2 协议冻结项

| Action | 方向 | 最小载荷 | 成功结果 |
| --- | --- | --- | --- |
| `BootNotification` | 桩→平台 | `deviceId`、`model` | `status=Accepted`、`heartbeatIntervalSec=10` |
| `Heartbeat` | 桩→平台 | `sentAt` | `currentTime` |
| `StatusNotification` | 桩→平台 | `status`、`occurredAt` | 空对象 |
| `MeterValues` | 桩→平台 | `powerW`、`energyMilliWh`、`sampledAt` | 空对象 |
| `RemoteStartTransaction` | 平台→桩 | `transactionId` | `status=Accepted/Rejected` |
| `RemoteStopTransaction` | 平台→桩 | `transactionId` | `status=Accepted/Rejected` |
| `RemoteReset` | 平台→桩 | `reason` | `status=Accepted/Rejected` |

时间字段使用 UTC Unix 秒，电量使用整数毫瓦时，功率使用整数瓦。`RemoteReset` 成功响应后停止计量，进入 `Booting`，2 秒后断开并进入正常重连/`BootNotification` 流程。

### 3.3 模块分解

| 模块 | 规划产物 | 职责 | 依赖 | 验证 |
| --- | --- | --- | --- | --- |
| DL-01 帧编解码 | `core/message_frame.*` | 严格解析三种数组帧，验证类型、长度、ID 和字段类型 | Qt Core | 往返、非法 JSON、缺字段和超限测试 |
| DL-02 请求调度 | `core/action_dispatcher.*`、`pending_request_table.*` | Action 路由，生成唯一 ID，匹配响应，处理超时、错误和断线取消 | DL-01 | 并发不串号、未知 ID 和超时测试 |
| DL-03 连接会话 | `core/device_connection.*`、`reconnect_policy.*` | 包装已接入或主动连接的 WebSocket，生命周期、发送队列、单调时钟活性和封顶退避 | DL-01～02、Qt WebSockets | 断线、半开、退避序列和恢复测试 |
| DL-04 设备注册 | `core/device_registry.*` | 管理多连接、设备 ID 绑定、最后心跳、在线/离线状态和重复 ID 拒绝 | DL-03 | 5 设备隔离和旧/新会话竞态测试 |
| PS-01 桩端业务 | `pile_app/pile_controller.*` | 启动注册、心跳、状态机、计量与远程命令 | DL-01～03 | 纯状态机和虚拟时间测试 |
| PS-02 桩端界面 | `pile_app/ui/pile_main_window.*` | 状态展示、数字计量、通信暂停/恢复、故障注入/恢复 | PS-01 | 加载、错误、恢复和重复点击烟雾测试 |
| PF-01 平台业务 | `platform_app/platform_controller.*` | 接入连接，处理设备 Action，下发命令，聚合状态和计量 | DL-01～04 | 多设备命令与离线测试 |
| PF-02 平台界面 | `platform_app/ui/platform_main_window.*` | 设备列表、最后心跳、状态/计量刷新和三种远程操作 | PF-01 | 空列表、5 设备、离线和命令失败 UI 测试 |
| DL-05 集成与运行 | `tests/`、`README.md`、`docs/protocol.md` | 端到端场景、参数说明、结构化日志和证据收集 | 全部模块 | 5 桩并发全链路验收 |

`core/` 中不得出现 `Pile`、`Charger`、`BootNotification`、`MeterValues` 等业务类型或 Action 常量；Action 名称和状态机分别属于 `pile_app` 和 `platform_app`。

### 3.4 实施路径

1. **S0：独立骨架与协议冻结**
   - 创建独立 CMake/CTest，完成 `docs/protocol.md`、DL-01 及非法帧测试。
   - 退出条件：单独配置和构建成功，根工程无 WebSockets 时仍可构建。
2. **S1：通用请求与连接核心**
   - 实现 DL-02～03，计时器和 ID 生成器允许测试注入，不依赖真实等待。
   - 退出条件：响应匹配、超时、断线取消、退避和半开检测全部自动化。
3. **S2：桩端纵向切片**
   - 实现 PS-01，先通过无 GUI 测试完成 Boot→Idle→Charging→Idle 及 Faulted 分支，再增加 PS-02。
   - 退出条件：桩端可独立连接测试平台，通信暂停不会快速重连。
4. **S3：平台多设备切片**
   - 实现 DL-04、PF-01，再增加 PF-02。底层 socket 索引与已注册设备 ID 分开，Boot 成功后才绑定。
   - 退出条件：5 个设备同时在线，单个设备的状态、心跳和命令不影响其他设备。
5. **S4：故障和并发验收**
   - 实现 DL-05，对 5 设备同时下发命令，覆盖计量刷新、未知 ID、30 秒离线、恢复和远程重启。
   - 退出条件：单元、协议、集成和 GUI 烟雾测试均由 CTest 注册，验收证据已归档。

## 4. 交付顺序与并行边界

```text
UC-U-05 真实头像闭环 → AV-02 图片处理 → AV-03/04 摄像头 → AV-05/06 集成 → AV-07 验收

S0 协议/骨架 → S1 通信核心 → S2 桩端 ─┐
                                      ├→ S4 联调验收
                             S3 平台 ─┘
```

- 头像任务与当前 `apps/user` 改动重叠，应由同一分支或明确文件所有者串行实施。
- 模拟器 S0～S1 可与头像任务并行；S2 与 S3 只在 S1 公共接口冻结后并行。
- 不在本路径中实施主工程远程重启联动。联动是新的跨进程契约任务，不能作为模拟器首次验收的隐式范围。

## 5. 通用完成门禁

每个模块标记完成前必须同时满足：

- 产物存在且没有使用占位页、空测试或演示数据代替真实链路；
- 正常、空数据、失败和恢复均有可重复验证；
- 相关最小构建、CTest、烟雾测试、`git diff --check` 和 `./scripts/check.sh` 通过；
- 需求矩阵、需求追踪、依赖清单和运维说明与真实结果同步；
- 未安装可选模块时，正式 `ncs_user`、`ncs_admin`、`ncs_server` 目标的配置、构建和烟雾测试不受影响。
