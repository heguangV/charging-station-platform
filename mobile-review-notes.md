# apps/mobile 审核导读（本地笔记，未跟踪文件，请勿提交）

> 用途：配合人工审核安卓用户端（Qt Quick 实验目标）使用。行号基于 2026-09-09 晚间工作区，
> 代码变更后会漂移，仅作定位参考。
> 定位提醒：按 AGENTS.md，`apps/mobile/` 是**实验目标，默认不构建**（需根工程
> `NCS_BUILD_ANDROID_EXPERIMENT` 或本目录独立配置 Qt Android kit），不得作为正式功能入口。

---

## 0. 模块地图

```
apps/mobile/                  独立 CMake 工程 NcsMobile（Android arm64，Qt 6.8）
├── CMakeLists.txt            qt_add_executable: main.cpp + mobile_api/profile/review/location.cpp
│                             qt_add_qml_module: Main.qml + StationDetail.qml（URI NcsMobile）
├── main.cpp                  入口：QtWebView 初始化 + mobileApi 上下文属性
├── mobile_api.h/.cpp         MobileApi（QObject）：网络层 + 充电/订单/站点业务编排
├── mobile_profile.cpp        MobileApi 的资料/头像/充值/注销/登出部分（按 TU 拆分）
├── mobile_location.cpp       定位（GPS/模拟）与导航查询、相机权限
├── mobile_review.cpp         UC-U-12 订单评价（GET 查询 + POST 提交，固定幂等键）
├── mobile_route_query.h      路线查询参数构造（wgs84/gcj02 坐标类型区分）
├── mapconfiguration.h/.cpp   腾讯地图 Key/.env 配置读取（注意：当前不在构建清单里，见 §7）
├── Main.qml                  唯一主窗口：8 个页面 + 5 个弹层 + 相机（约 1950 行）
├── StationDetail.qml         场站详情组件（含 UI 契约测试锚点 objectName）
├── qml/Main.qml              旧地图原型，**不在构建中**（引用不存在的上下文属性）
├── resources/tencent-map.html  内嵌腾讯地图 demo 页（Key 占位符替换）
├── android/AndroidManifest.xml 权限与明文流量声明
└── run_android.sh            USB 联调辅助（adb reverse + 启动/安装）
```

依赖：Qt6 Core/Gui/Qml/Quick/QuickControls2/Network/**Multimedia/Positioning/WebView**；
`NCS_HAS_POSITIONING` 宏由 CMake 定义（无 Positioning 构建时定位功能降级提示）。
UI 风格沿用 apps/user（浅绿卡片、圆角、底部导航、中文提示）。

---

## 1. 入口与构建（main.cpp / CMakeLists.txt）

- `main.cpp`：`QtWebView::initialize()` → Basic 样式 → 构造 `MobileApi` 并以
  `mobileApi` 上下文属性暴露给 QML → 加载 `qrc:/qt/qml/NcsMobile/Main.qml`。
- CMake：Android 包名 `com.ncs.charging`、应用名"NCS 充电"、版本 1.0(1)；
  `QT_ANDROID_PACKAGE_SOURCE_DIR` 指向 android/ 子目录（自定义 Manifest）。
  README 明确：无 Qt Multimedia 时不得构建该目标（拍照是必需能力）。

---

## 2. MobileApi 基础设施（mobile_api.cpp:29-136）

- **服务地址**：构造时读 `NCS_API_BASE_URL` 环境变量，缺省回退 QSettings 里的
  `serverUrl`（再缺省 `http://127.0.0.1:18443/api/v1`）；`manager_` 禁用系统代理。
- `request()`：统一构造请求——Bearer 令牌、`Connection: close`（注释：adb reverse 隧道下
  keep-alive 流可能被对端关闭且 Qt 不自动重试 POST，逐请求新建连接最稳）、15 秒传输超时、
  仅同源重定向。
- `parseReply()`：统一响应解析——
  - **generation 门闩**：回复上的 generation 属性 ≠ 当前 `generation_`（已登出/清会话）直接丢弃；
  - 成功要求 `reply->error()==NoError` 且 JSON 含 `success:true`；
  - HTTP 401 → `clearSession()` + "登录已过期"（全局会话失效处理）；
  - 错误文案优先取服务端 `userMessage`，缺省给"无法连接服务端…"；
  - 携带 `mutationScope` 属性的回复成功后从 `mutationKeys_` 移除幂等键。
- `watch()`：登记 generation + 计数 pending_（busy 状态来源）。
- `postJson()`：**客户端幂等键机制**——scope = 路径+请求体 JSON，同 scope 复用已生成的
  UUID（`Idempotency-Key` 头），失败后重试沿用同键、成功后移除；busy 时静默忽略新提交。
  与服务端 IdempotencyService（scope+UUID 键、同键同摘要重放）的契约正好咬合。

---

## 3. 业务功能（mobile_api.cpp）

### 登录（:137-192）
- `requestCode()`：11 位手机号正则校验 → POST `/user/auth/sms/code`（purpose=LOGIN）；
  服务端返回 `developmentCode` 时直接展示（仅开发模拟短信模式会返回）。
- `login()`：POST `/user/auth/login/sms`，deviceId 固定 `"android-mobile"`；
  成功存 token、拉资料与站点。
  > 审读提示：deviceId 固定值意味着所有安装实例共享同一"设备"身份——SessionManager 的
  > 同设备替换策略下，第二台手机登录会顶掉第一台的会话；一台设备内也无法多会话并存。
  > 这是简化而非缺陷，但审核会话管理需求时值得确认。

### 站点（:193-275）
- `loadStations(keyword)`：`pageSize=100` + 当前坐标；**请求序号（serial）竞态防护**
  ——过期回复直接丢弃；关键字再按名称/地址做一次客户端过滤（服务端也支持 keyword）。
- `loadStationChargers(stationId)`：`page=1&pageSize=50`，同样 serial 防护。
- `loadStationReviews(stationId)`：UC-U-12 评论墙，busy/error/空三态齐全，时间戳转 "MM-dd"。

### 充电流程（:289-453）
- `requestCharge()`：已有活跃 flow 时本地拦截（"请先处理当前未结算的充电订单"，
  对应服务端 ActiveFlowExists）；preferredChargerId 为 0 时显式传 null。
- `confirmCharge()/startCharge()/cancelCharge()`：分别 POST 报价确认/开始/取消，
  都带 `flowVersion` 乐观锁；取消 reasonCode=`USER_CANCELLED`。
- `settleCharge()`：POST 结算（reasonCode=`USER_FINISHED`）；成功后存小票、清 flow、
  刷新订单与资料，文案明确"已结束充电，请在订单中确认扣款或发起申诉"——**不把结算当扣款成功**
  （UC-U-09 语义：结算只冻结金额到状态 100）。
- `loadFlowProgress()`：先拉流程详情，status≥60 → 刷活跃流程+订单+资料三件套并停止；
  40/50 再拉 `/progress` 合并进 flow_。由 QML 的 1 秒 Timer 驱动（仅前台且 flow 存在时运行）。

### UC-U-09 确认扣款与申诉（:369-401）
- `confirmOrder(orderNo)`：POST `/user/orders/{orderNo}/confirmation`；成功更新小票、
  刷订单/资料、发 `orderConfirmed` 信号（QML 关弹窗用）。
- `appealOrder(orderNo, reason)`：客户端先做 1~500 字校验（`toUcs4().size()` 数码点）→
  POST `/user/orders/{orderNo}/appeals`；成功发 `orderAppealed`。
- 两者都走 `postJson` 的幂等键机制（重复提交同键重放）。

### 订单与服务器设置
- `loadOrders()`：`pageSize=100&sort=-createdAt`；`loadOrder(orderNo)`：详情进小票。
- `configureServer()`：仅未登录可改；URL 校验——**必须 https，http 仅回环地址**、
  禁 userInfo/query/fragment、路径必须为 `/api/v1`；成功写 QSettings 并清连接缓存。

---

## 4. 资料域（mobile_profile.cpp）

- `loadProfile()`：GET `/user/me`——记 `profileVersion_`（昵称更新的乐观锁入参）、
  注册日期本地化、**手机号取 `phoneMasked`**（脱敏字段）；`avatarUrl` 非空再拉
  `/user/me/avatar/content` 转 base64 存 `avatarData_`（带 generation 门闩）。
- `uploadAvatar(path)`：本地文件 ≤5 MiB → 尺寸校验（≤16000）→ 按比例缩到 1024 →
  **中心裁剪 512×512** → JPEG 质量 80（≤200 KiB）→ multipart POST `/user/me/avatar`；
  上传中置 `avatarUploading_`；拍摄临时文件与上传源相同时顺手删除。
- `logout()`：POST `/user/auth/logout` 后 `clearSession()`。
- `clearSession()`：`generation_++`（作废所有在途回复）+ 各请求序号自增（作废 serial 门闩）
  + 清空全部状态字段并逐个发信号——登出/注销/401 的统一收口。
- `updateNickname()`：1~20 字（UCS4 码点），PUT `/user/me` 带 `version`（服务端
  VersionConflict 时经 parseReply 显示服务端 userMessage）。
- `recharge(amount)`：正则（≤5 位整数+最多 2 位小数）→ 换算分（`leftJustified(2,'0')`
  处理 "5.5" → 550）→ 范围 1~1000000 分 → POST `/user/wallet/recharges`。
- `requestDeletionCode()`：用登录手机号发 `RESET_PASSWORD` 用途验证码（注销核身复用）。
- `deleteAccount(code)`：DELETE `/user/me`，body `{confirm:true, password:null, smsCode:code}`；
  成功 `clearSession()`。

---

## 5. 定位与导航（mobile_location.cpp）+ route_query

- `loadRoute(stationId, mode)`：路径参数由 `routeQuery()` 构造——有地址用 keyword；
  坐标则区分 `coordinateType=wgs84/gcj02`（**GPS 原始定位是 WGS84，模拟位置是地图坐标系**，
  头注释），mode 缺省 driving；serial 防护。
- `locateDevice()`：`QLocationPermission`（Precise）未决则请求、拒绝则提示可改用模拟位置；
  `createDefaultSource` 不可用同样降级；取到定位后做**时效与精度校验**
  （时间戳年龄 -30~120 秒、水平精度 ≤2000 米，`mobile_location.cpp:126-133`），
  通过才更新坐标并置 `gpsOrigin_=true`。
- `setLocation(region, address)`：5 个内置模拟区域（北京中心/中关村/北京南站/石景山/通州）
  的硬编码坐标，置 `gpsOrigin_=false`，立即刷新站点。
- 相机：`cameraPermissionGranted()/requestCameraPermission()`（Android 6.0+ 运行时危险权限）、
  `avatarCapturePath()`（CacheLocation 下固定文件名）、`discardCapture()`。

---

## 6. 订单评价（mobile_review.cpp，UC-U-12）

- `beginReview(orderNo)`：生成**本次会话固定的 Idempotency-Key**（`reviewKey_`）→
  先 GET `/user/orders/{orderNo}/review` 查已有评价（幂等重放口径：失败重试沿用同键，
  与 README 描述一致）。
- `submitReview(rating, content)`：1~5 星、1~500 码点、无 NUL 校验 → POST 同路径 + 幂等键。
- `handleReviewReply()`：reviewRequest 序号门闩 + 401 特判 + generation 门闩三层防护；
  维护 `reviewInfo_` 的 existing/submitted/busy/rating/content/error 状态机；
  已存在评价时 UI 只读。

---

## 7. mapconfiguration（.env 地图配置）——当前不在构建中

> 审读提示（重要）：`mapconfiguration.h/.cpp` **未出现在 CMakeLists 的
> `qt_add_executable` 源列表里**（CMakeLists.txt:11），当前**不会被编译进 ncs_mobile**。
> 它是 apps/dashboard 同名模式的移植（读 `.env` 的 TENCENT_MAP_JS_KEY/ORIGIN/PAGE_URL、
> 渲染 resources/tencent-map.html 模板），且 `qml/Main.qml` 旧原型引用了它的上下文属性。
> 主界面（Main.qml）实际采用"服务端返回 browserUrl + WebView 打开"的方案，
> 不再内嵌地图 Key。审核时确认这份代码是"待清理"还是"待启用"即可。

- 语义（若启用）：优先 `NCS_ENV_FILE` 指定路径，否则依次找 cwd / `NCS_PROJECT_ROOT`
  宏目录 / 可执行目录下的 `.env`；环境变量优先于文件值；`replace_me` 视为未配置；
  `mapHtml()` 把 Key 百分号编码后替换模板占位符。

---

## 8. Main.qml（主窗口，约 1950 行）

**页面路由**：`window.page` 字符串驱动 StackLayout 的 8 个索引——
login / home / detail / orders / profile / charge / receipt / navigation；
`go()` 统一入口（进 charge 前拉活跃流程，home/orders/profile 各自触发刷新）；
`onClosing` 拦截安卓返回键：非 home/login 页一律先回 home。

**全局组件**（`:29-63`）：`Field`（输入框）、`Sheet`（圆角弹层）、`ActionButton`。

**Connections（`:119-153`）**：
- `onChargersChanged`：所选桩不再是空闲桩则自动清空选择；
- `onCodeSent`：验证码 60 秒倒计时（codeTimer）；
- `onSessionChanged`：登录态切换页面，登录后拉活跃流程；
- `onReceiptChanged`：小票非空自动跳 receipt 页；
- `onOrderConfirmed/onOrderAppealed`：UC-U-09 专用信号，只关对应弹窗（避免与无关刷新串扰），
  申诉成功还会刷新当前小票。

**Timers（`:154-172`）**：验证码倒计时（1s）；充电进度轮询（1s，条件
`loggedIn && flow.flowNo && 应用前台`——注意**不限定在 charge 页**，挂后台自动暂停）；
搜索防抖（350ms）。

**各页面要点**：
- **home**：搜索框（防抖）、模拟定位下拉 + "使用当前位置"、活跃流程快捷入口、站点卡片
  （价格富文本、空闲/总数、距离）。
- **orders**：订单卡片——statusText=="待用户确认"（100）时显示"确认完成并扣款 /
  不满意，发起申诉"双按钮；"申诉待审核"（110）显示只读提示条；已完成订单显示"评价"按钮。
- **profile**：头像（Canvas 圆形裁剪 base64 图）、昵称编辑、余额、充值/退出/注销入口。
- **charge**：状态横幅 + 电量/费用/功率三格 + 报价或排队位；按钮按 status 启停
  （确认报价=20、开始充电=30、结束充电=40、取消预约=10/20/30）。
- **receipt**：小票页——**金额标签按状态区分**：100 显示"应付金额"、110 显示"争议金额"、
  其余显示"支付金额"（`Main.qml:1068-1077` 注释：100/110 尚未扣款 paidCent 为 0）；
  100 状态时在页内直接给确认/申诉双按钮。
- **navigation**：起点地址输入（空则用模拟位置）、驾车/步行/公交三模式、路线摘要
  （`routeFallback` 时提示"路线服务降级，已提供本地距离"，`durationSecond` 为 0 时显示
  "预计时间未知"——与 core 回退语义咬合）、`browserUrl` 存在时 WebView 展开 +
  "使用地图应用继续导航"（`Qt.openUrlExternally`）、分步指引列表。
- **弹层**：serverDialog（服务器设置）、rechargeDialog、deletionDialog（注销两步验证码）、
  avatarChoice + cameraDialog（拍照/相册）、reviewDialog（星级+500 字）、
  confirmDialog（确认扣款前确认金额与欠费规则）、appealDialog（申诉原因）。
- **相机链**（`:1586-1945`）：打开即查权限→`camera.start()`；`MediaDevices` 切换镜头；
  `ImageCapture.captureToFile(avatarCapturePath())`；`onImageSaved` 回填预览；
  确认上传后 `uploadAvatar`；关闭/异常都 `discardCapture()` 清理临时文件。
- **全局**：错误横幅（除 login/profile/charge 页外浮层展示）、BusyIndicator、底部 4 tab。

---

## 9. StationDetail.qml（场站详情组件）

- **属性**：station/chargers/selectedId/busy/hasActiveFlow/errorMessage/评论墙三态；
  **信号**：ordersRequested/backRequested/navigationRequested/reserveRequested/
  activeFlowRequested/refreshRequested/chargerSelected(id,type)——页面逻辑全部上抛给 Main.qml。
- **卡片区**：站点信息卡（名称、空闲 x/y、距离、导航按钮）；价格卡（当前参考价大字 +
  电费/服务费小卡 + "最终单价以预约后确认的报价为准"免责）；桩列表卡——默认显示 3 根 +
  "展开更多"（objectName `expandChargers`），每桩可选（仅空闲，重复点击取消，
  objectName `chargerChoice<id>` 供 UI 契约测试定位）；评论墙卡（加载/失败/空三态 +
  "查看我的订单"入口）。
- **底部**："一键导航" + 预约按钮（文案三态：继续当前充电 / 处理中… / 预约所选电桩 /
  请选择空闲电桩；objectName `reserveButton`；深色底）。
- 详情滚动区 objectName `stationDetailScroll`——这些 objectName 是 tests/admin_ui_contract_test
  类似的 UI 契约测试锚点，改名会破坏测试。

---

## 10. 其余资源

- **qml/Main.qml**：旧地图原型（蓝色工具栏 + WebView 地图 + 占位四 tab），
  引用 `mapConfiguration`/`ncsSmokeTest` 上下文属性——**不在 CMake QML_FILES 里，不参与构建**。
  > 审读提示：与 mapconfiguration.cpp 同属遗留；若确认废弃建议一并清理，避免误导。
- **resources/tencent-map.html**：内嵌地图 demo 页（5 个硬编码北京站点标记、错误横幅、
  `__TENCENT_MAP_JS_KEY__` 占位符）；只被 mapconfiguration::mapHtml() 读取。
- **android/AndroidManifest.xml**：权限 INTERNET/CAMERA/NETWORK_STATE/粗+精定位；
  camera `required=false`；`allowBackup=false`；**`usesCleartextTraffic="true"`**
  （注释：开发联调期允许明文 HTTP 18443，正式 HTTPS 后应关闭）。
  > 审读提示：`configureServer()` 在 UI 层只允许 https（http 仅回环），但 Manifest 全局
  > 允许明文流量——发布前需两处一致收紧；这是当前分支明确标注的临时态。
- **run_android.sh**：校验 adb 与本机服务健康（`/system/health/ready`）→
  `adb reverse tcp:18443 tcp:18443`（手机访问 127.0.0.1:18443 即宿主机）→
  可选 `--install` 装 APK → 启动 Activity。

---

## 11. 审读提示汇总

1. **构建外文件**：mapconfiguration.h/.cpp、qml/Main.qml、resources/tencent-map.html
   当前都不参与构建（§7、§10）——确认去留。
2. **deviceId 固定 "android-mobile"**：跨设备会话互顶、同设备单会话（§3）。
3. **Manifest 明文流量**与 UI 校验规则不一致（§10）。
4. `postJson` busy 时**静默丢弃**新提交（无任何提示）——快速双击第二个不同操作会被吞；
   靠 busy 的 UI 禁用兜底，审核交互时注意。
5. 充电进度轮询在**任意前台页面**都运行（只要 flow 存在），不止 charge 页——
   电量低时 1s 间隔网络请求的功耗可评估。
6. 头像上传前的客户端压缩（512×512/JPEG q80/≤200KiB）与服务端限制的对应关系
   值得对照 apps/user 的实现确认一致。
7. 充值/确认扣款/评价都走幂等键；**申诉也走同一机制**（appealOrder 经 postJson）——
   失败重试沿用同键，符合"每单一申诉"的幂等语义。
8. UC-U-09 的文案口径（"冻结金额待确认"、"审核期间不扣款"）在 settleCharge、
   订单卡、小票页、确认弹窗、申诉弹窗五处重复出现——改文案时需全量同步。

---

## 12. 建议的审核顺序

1. CMakeLists + main.cpp → 确认构建边界与上下文属性。
2. mobile_api.cpp §2 基础设施 → 幂等键、generation/serial 双门闩、401 收口。
3. 充电流程 + UC-U-09 → 与 core 新状态机（100/110）和服务端 order_payment_routes 对照。
4. mobile_profile/review/location → 输入校验与权限流程。
5. Main.qml → 页面路由、状态驱动按钮、相机链。
6. 遗留文件（mapconfiguration、qml/Main.qml）→ 处置决定。
