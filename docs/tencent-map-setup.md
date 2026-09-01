# 腾讯地图接入与密钥管理

## 1. Key 分工

项目使用两个互不混用的配置项：

- `TENCENT_MAP_JS_KEY`：仅供腾讯 JavaScript API GL 渲染地图。该 Key 在网页运行时对用户可见，必须在腾讯位置服务控制台限制来源和额度。
- `TENCENT_MAP_SERVER_KEY`：仅供 PC 服务器/管理端调用地理编码和路线规划 WebService。不得传给用户端、HTML 或日志；按腾讯控制台能力限制允许来源或出口 IP。

真实值写入仓库根目录 `.env` 或服务器进程环境。Git 仅提交 `.env.example`。

## 2. 本机开发

在仓库根目录创建配置：

```bash
cp .env.example .env
chmod 600 .env
```

填写 `TENCENT_MAP_JS_KEY` 后，当前 Linux Qt Widgets 用户端通过 `QWebEngineView` 加载内置 HTML 地图页。腾讯控制台中的 JavaScript API 允许来源需要包含本机开发来源；默认配置为：

```dotenv
TENCENT_MAP_JS_ORIGIN=http://localhost/
```

如果控制台配置了其他允许来源，应把该变量改为完全一致的 `http://` 或 `https://` 地址。配置加载优先级为：

1. 当前进程环境变量；
2. `NCS_ENV_FILE` 指定的文件；
3. 仓库根目录或程序目录中的 `.env`。

程序不会输出 Key。Widgets 地图组件只读取 `TENCENT_MAP_JS_KEY` 和 `TENCENT_MAP_JS_ORIGIN`；`TENCENT_MAP_SERVER_KEY` 只由 PC 服务器/管理端读取。

若页面显示“当前产品鉴权失败”，应进入 Key 设置确认已经单独开启 JavaScript API GL，并核对该 Key 的允许来源。WebService API 的开关不能替代 JavaScript API GL。

## 3. 远期 Android 方案

Android/QML 不属于当前验收范围。未来恢复 Android 版本时不得把 `.env` 或 WebService Key 打入 APK，可由受控服务提供地图页面，并在 App 运行环境中设置：

```dotenv
TENCENT_MAP_PAGE_URL=https://你的服务地址/mobile/map
```

远期移动端加载该 URL。地图页面仍会向浏览器暴露 JS Key，因此必须配置允许来源、调用额度和告警。

## 4. 构建验证

```bash
/home/bit/Qt/6.8.3/gcc_64/bin/qt-cmake -S . -B build-qt68 -G Ninja
cmake --build build-qt68 --target codex-qt-demo
```

正常桌面运行：

```bash
build-qt68/codex-qt-demo
```

当前地图原型需要从 QML 骨架迁移到 Qt Widgets `QWebEngineView` 后再作为交付功能。地图失败时界面必须保留可见错误提示；站点列表仍使用预置北京坐标和 Haversine 距离作为降级能力。

## 5. 提交前检查

```bash
git check-ignore -v .env
git status --short
git ls-files .env
```

`.env` 应被忽略，`git ls-files .env` 应无输出。若 Key 曾进入 Git 历史，必须立即在腾讯位置服务控制台重新生成，单纯删除文件或补充 `.gitignore` 不足以消除泄露。
