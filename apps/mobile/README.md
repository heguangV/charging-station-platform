# NCS 安卓用户端

这是基于 Qt Quick、Qt Quick Controls、Qt Network 和 Qt Multimedia 的 Android 用户端。界面沿用 `apps/user` 的浅绿色卡片、圆角、底部导航和中文提示风格。

当前页面包括短信登录、附近站点、订单入口、个人资料、余额、拍照添加头像和充电后评价。登录、站点、资料、头像上传、订单与评价使用现有 `/api/v1` REST 契约；拍照使用 Android 摄像头权限和 Qt Multimedia，照片通过现有头像 multipart 接口上传。

充电后评价（UC-U-12）：订单列表中已完成订单提供"评价"入口，填写 1～5 星与 1～500 字评语后经 `GET/POST /api/v1/user/orders/{orderNo}/review` 提交；每单仅一条评价且不可修改，提交失败保留草稿并沿用同一 `Idempotency-Key` 重试，已评价订单只读展示。

建议在 Android 模拟器中将 `NCS_API_BASE_URL` 指向 `https://10.0.2.2:8443/api/v1`；真机联调应使用受控 HTTPS 地址。构建时启用根工程的 `NCS_BUILD_ANDROID_EXPERIMENT`，或从本目录单独配置 Qt Android kit，例如：

```bash
Qt/6.8.3/android_arm64_v8a/bin/qt-cmake -G Ninja -S apps/mobile -B build/android \
    -DQT_HOST_PATH=Qt/6.8.3/gcc_64 \
    -DANDROID_SDK_ROOT=$ANDROID_SDK_ROOT \
    -DANDROID_NDK_ROOT=$ANDROID_SDK_ROOT/ndk/26.1.10909125 \
    -DANDROID_ABI=arm64-v8a
cmake --build build/android --target ncs_mobile_make_apk
```

未安装 Qt Multimedia 时不应构建该 Android 目标，因为拍照是该目标的必需能力。
