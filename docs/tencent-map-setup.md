# 腾讯地图接入

本文只说明腾讯地图的本地配置、验证和排错。地图功能与降级行为以 SRS 的 `UC-U-02`～`UC-U-04` 为准，构建环境见仓库 README。

## 1. Key 分工

| 配置项 | 用途 | 可见范围 |
| --- | --- | --- |
| `TENCENT_MAP_JS_KEY` | Vue Web 用户端（`apps/user`）的 JavaScript API GL 地图渲染；由 Vite 在开发/构建时从仓库根 `.env` 注入 `import.meta.env.TENCENT_MAP_JS_KEY` | 会在页面运行时对用户可见，必须限制来源和额度；不得与服务端 Key 混用 |
| `TENCENT_MAP_SERVER_KEY` | 服务端 WebService：地理编码、POI 周边检索（`/ws/place/v1/search`）、驾车/步行/公交路线规划 | 仅 Crow 服务端读取（`infrastructure/map`），不得发送给客户端、写入日志或进入前端构建产物 |

两个 Key 不得混用。分别在腾讯位置服务控制台启用所需 API，并按控制台能力配置来源、出口 IP、配额和告警。前端的 JS Key 只从仓库根 `.env` 读取（`vite.config.js` 的 `envDir` 指向仓库根，`envPrefix` 仅放行 `VITE_*` 与 `TENCENT_MAP_JS_KEY`），因此 Agent 的 POI/路线能力与前端地图显示各自使用各自的 Key，互不越界。

## 2. 本地配置

在仓库根目录创建仅本机可读的 `.env`：

```bash
touch .env
chmod 600 .env
```

按实际用途填写：

```dotenv
TENCENT_MAP_JS_KEY=你的JavaScriptKey
TENCENT_MAP_SERVER_KEY=你的WebServiceKey
TENCENT_MAP_JS_ORIGIN=http://localhost/
```

也可以使用仓库脚本交互式写入。脚本不会回显 Key，会原子更新三个地图字段并把权限设为 `600`：

```bash
./scripts/configure-local-map.sh
./scripts/configure-local-map.sh --check
```

需要操作其他本机配置文件时可显式设置 `NCS_ENV_FILE=/绝对路径/test.env`。脚本拒绝写入符号链接，真实 Key 不作为命令行参数传入，避免进入 shell 历史和普通进程参数。

配置优先级从高到低为：命令行参数（`--tencent-map-key`）、进程环境变量（`NCS_TENCENT_MAP_KEY`）、`NCS_ENV_FILE` 指定文件、仓库或程序目录中的 `.env`。文件中除腾讯地图服务端 Key 使用 `TENCENT_MAP_SERVER_KEY` 外，其余条目与进程环境变量同名。`.env` 支持注释（`#`）、空行、`export` 前缀与引号包裹的值，同一键首次出现生效，空值视为未设置；`NCS_ENV_FILE` 指向的文件缺失或不可读时服务端拒绝启动。真实 Key 不得写入源码、普通日志或 Git。

## 3. 验证

1. 确认 `.env` 被 Git 忽略：`git check-ignore -v .env`。
2. 启动服务端，验证 WebService Key 可完成一次北京地址地理编码，并分别取得驾车、步行、公交路线。
3. 启动 Web 用户端（`cd apps/user && npm run dev`）并登录，验证附近站点在地图上以 Marker 显示、定位按钮可用、站点详情可进入导航。
4. 在 AI 助手页提问（例如“充电站旁边哪里可以喝咖啡”），验证 POI 结果来自 Server Key 调用的 WebService，且响应中的 `provider` 与 `fallback` 与实际情况一致。
5. 临时断网或使用无效 Server Key，验证此时才显示 `LOCAL_FALLBACK`、浏览器导航入口和 Haversine 距离；Agent 的 `route.provider` 变为 `LOCAL_FALLBACK` 且 `degraded=true`，站点结果仍返回。
6. 使用无效 JS Key 或缺省 JS Key，验证地图区域显示失败态与配置指引，站点列表与充电流程不受影响。

## 4. 常见问题

| 现象 | 检查项 |
| --- | --- |
| 页面提示鉴权失败 | JS Key 是否启用 JavaScript API GL；允许来源是否与 `TENCENT_MAP_JS_ORIGIN` 完全一致 |
| 地理编码失败 | 是否误用了 JS Key；WebService API 是否启用；服务端出口限制是否正确 |
| 路线只显示本地降级 | Server Key 是否同时启用了驾车、步行和公交路线规划；配额、出口限制和服务端超时是否正常 |
| Web 页面地图空白并提示未配置 Key | 仓库根 `.env` 是否设置 `TENCENT_MAP_JS_KEY`；前端是否在设置后重新执行 `npm run dev` 或 `npm run build`（该值在构建时注入） |
| Web 地图加载失败 | JS Key 来源限制是否包含当前访问来源；网络是否可达 `map.qq.com`；浏览器是否支持 WebGL |
| Agent 未返回 POI | 是否配置 `TENCENT_MAP_SERVER_KEY`；是否启用 WebService 周边检索；配额与出口 IP 限制是否正常 |
| 程序未读取配置 | 环境变量、`NCS_ENV_FILE` 和 `.env` 路径是否正确；进程是否在修改后重启 |
| Key 曾被提交 | 立即在控制台重新生成；删除文件或补充 `.gitignore` 不能消除 Git 历史泄露 |
