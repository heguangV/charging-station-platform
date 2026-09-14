# apps/mobile：Qt Quick 移动用户端

本目录是 NCS 平台的 Qt Quick 用户客户端，主要面向 Android，也可使用桌面 Qt kit 构建以调试界面。它通过现有 `/api/v1` REST 接口访问服务端，负责移动交互和展示，不直接读取 SQLite，也不在本地决定订单金额、扣款或审核结果。

根工程通过 `NCS_BUILD_ANDROID_EXPERIMENT` 控制此目标，默认不构建；正式桌面用户端仍位于 [`apps/user`](../user/)。移动端可单独配置 CMake，避免 Android 构建同时带入整个桌面端和服务端工程。

## 功能概览

- 短信登录、服务器地址配置与会话失效处理。
- 附近站点、设备选择、报价确认、预约、开始/结束充电及活动流程恢复。
- 历史订单、小票、用户确认扣款和填写原因申诉。
- 个人资料、昵称、余额充值、账号注销、头像拍摄与上传。
- 设备定位、模拟位置选择、路线摘要、WebView 地图及外部导航入口。
- 充电后评价和场站评论墙。

界面沿用桌面用户端的浅绿色卡片、圆角、底部导航和中文提示。已有代码不等于真机验收全部完成，具体状态见[需求追踪表](../../docs/requirements-traceability.md)。

## 架构和数据流

```text
Main.qml / StationDetail.qml
  → mobileApi 的 Q_INVOKABLE 方法
  → MobileApi / QNetworkAccessManager
  → ncs_server REST 接口
  → JSON 转为 QVariantMap / QVariantList
  → Q_PROPERTY 与变更信号更新 QML
```

[`main.cpp`](main.cpp) 初始化 WebView 和应用样式，创建 `MobileApi`，通过 QML context property 注册为 `mobileApi`，再加载 `qrc:/qt/qml/NcsMobile/Main.qml`。`MobileApi` 既是网络适配层，也是暴露给 QML 的状态对象。

请求使用 Bearer 令牌和统一响应解析，`busy` 等属性控制加载状态。会话代次和部分查询序号用于忽略过时响应，避免切换账号或页面后旧请求覆盖新数据。当前主要通过 REST 和 QML 定时刷新获取变化，充电进度由定时器请求；服务端支持 WebSocket 不代表移动端已接入全部实时事件。

## 文件导航

| 文件 | 主要职责 |
| --- | --- |
| [`Main.qml`](Main.qml) | 当前入口：页面切换、登录、订单、充电控制、资料、拍照、导航和交互弹窗 |
| [`StationDetail.qml`](StationDetail.qml) | 场站详情、设备选择、充电入口和评论墙；通过属性接收状态，通过信号请求操作 |
| [`mobile_api.h`](mobile_api.h) | `MobileApi` 的 QML 属性、方法、信号及请求状态声明 |
| [`mobile_api.cpp`](mobile_api.cpp) | 网络请求与响应、登录、站点设备、充电流程、订单确认和申诉 |
| [`mobile_profile.cpp`](mobile_profile.cpp) | 服务器配置、资料、余额、头像、账号与订单详情相关实现 |
| [`mobile_review.cpp`](mobile_review.cpp) | 评价读取/提交、草稿与幂等处理、场站评论查询 |
| [`mobile_location.cpp`](mobile_location.cpp)、[`mobile_route_query.h`](mobile_route_query.h) | 定位权限、模拟起点、路线查询参数、相机权限和拍摄临时文件相关方法 |
| [`android/AndroidManifest.xml`](android/AndroidManifest.xml) | Android 应用声明和权限配置 |
| [`run_android.sh`](run_android.sh) | USB 调试连接恢复、可选 APK 安装和启动 |
| [`CMakeLists.txt`](CMakeLists.txt) | `ncs_mobile`、`NcsMobile` QML 模块和 Android 包设置 |

目录还保留 `qml/Main.qml`、`mapconfiguration.*` 和 `resources/tencent-map.html` 等较早实现。当前 CMake 没有把这些文件列入 `ncs_mobile` 的源文件或 QML 资源清单；修改主界面应从**目录根部的 `Main.qml`** 开始，避免改到未使用的同名文件。

## 订单确认、申诉与评价

结束充电后先冻结账单，订单进入 100（待用户确认），尚未扣款。订单列表和小票页提供：

| 操作或状态 | 对应接口或行为 |
| --- | --- |
| 确认完成并扣款 | `POST /api/v1/user/orders/{orderNo}/confirmation` |
| 不满意，填写原因申诉 | `POST /api/v1/user/orders/{orderNo}/appeals` |
| 110：申诉待审核 | 显示待审核提示，不提供确认扣款入口；管理端负责审核 |
| 60：已完成且已结算 | 可进入原有评价模块 |

评价使用 `GET/POST /api/v1/user/orders/{orderNo}/review`，填写 1～5 星和 1～500 字文字；每单仅一条、不可修改，失败保留草稿并沿用同一幂等键重试，已评价内容只读展示。申诉期间不允许通过评论流程代替审核。业务资格与状态转换以服务端响应和[需求规格 UC-U-09/12](../../docs/01-requirements-specification.md)为准。

## 构建准备

移动目标实际依赖 **Qt 6.8+**：Core、Gui、Qml、Quick、QuickControls2、Network、Multimedia、Positioning、WebView。根工程的 Qt 6.2 最低声明不代表移动端可用 Qt 6.2 构建。Multimedia 是此目标的必需依赖。

Android 还需要与 Qt Android kit 匹配的桌面 Qt host kit、SDK/NDK、JDK 和 Ninja。以下命令从**仓库根目录**执行，先按本机安装位置设置路径：

```bash
export QT_ANDROID_KIT="$HOME/Qt/6.8.3/android_arm64_v8a"
export QT_HOST_PATH="$HOME/Qt/6.8.3/gcc_64"
export ANDROID_SDK_ROOT="$HOME/Android/Sdk"
export ANDROID_NDK_ROOT="$ANDROID_SDK_ROOT/ndk/26.1.10909125"

"$QT_ANDROID_KIT/bin/qt-cmake" -G Ninja -S apps/mobile -B build/mobile-android \
  -DQT_HOST_PATH="$QT_HOST_PATH" \
  -DANDROID_SDK_ROOT="$ANDROID_SDK_ROOT" \
  -DANDROID_NDK_ROOT="$ANDROID_NDK_ROOT" \
  -DANDROID_ABI=arm64-v8a
cmake --build build/mobile-android --target ncs_mobile_make_apk
```

NDK 示例沿用已有项目说明，实际版本应与所选 Qt kit 匹配。包名为 `com.ncs.charging`，APK 位置以构建输出为准。

## 服务地址与设备联调

地址来源依次为 `NCS_API_BASE_URL` 环境变量、`QSettings` 保存的 `serverUrl`、默认 `http://127.0.0.1:18443/api/v1`。地址须带 `/api/v1`。Android 通常通过应用中的服务器配置入口修改；在构建主机设置环境变量并不等于会写入安装后的 APK。

- **USB 真机联调**：先在虚拟机启动本机开发服务，把手机连接到虚拟机并授权 USB 调试，再运行 `apps/mobile/run_android.sh`。脚本检查 18443 就绪接口，执行 `adb reverse tcp:18443 tcp:18443`，随后启动应用。
- **安装后启动**：运行 `apps/mobile/run_android.sh --install /实际路径/app-debug.apk`。省略 APK 路径时，脚本使用内置的 `build-mobile-android/` 路径，可能与上面的示例构建目录不同。
- **多设备或自定义 SDK**：设置 `ANDROID_SERIAL`、`ANDROID_SDK_ROOT` 或 `ADB`。
- **Android 模拟器**：`10.0.2.2` 通常用于访问宿主机；HTTPS 地址还需与证书及信任链匹配。
- **正式环境**：使用受控 HTTPS 地址；默认回环 HTTP 和 USB 隧道属于开发联调方式。

## 验证与排障

独立测试工程位于 [`tests/mobile`](../../tests/mobile/)。在装有 Qt 6.8 桌面组件和 Qt Test 的环境中，从根目录执行：

```bash
"$QT_HOST_PATH/bin/qt-cmake" -G Ninja -S tests/mobile -B build/mobile-tests
cmake --build build/mobile-tests
ctest --test-dir build/mobile-tests --output-on-failure
```

契约测试、布局测试、APK 构建和真机验证是不同层次。相机、定位权限、WebView、USB 断连恢复和实际点击流程仍需在设备上检查。

无法连接时先核对 `/api/v1`、服务就绪状态和 USB 反向转发；拍照或定位不可用时检查运行时权限和硬件；地图为空时先看路线返回及降级提示。新增页面优先复用 `MobileApi` 的网络与会话处理，耗时工作不能阻塞 QML 主线程。

更多信息：[接口文档](../../docs/database-api.md)、[腾讯地图配置](../../docs/tencent-map-setup.md)、[研发实施指南](../../docs/development-guide.md)。
