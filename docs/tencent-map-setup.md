# 腾讯地图接入

本文只说明腾讯地图的本地配置、验证和排错。地图功能与降级行为以 SRS 的 `UC-U-02`～`UC-U-04` 为准，构建环境见仓库 README。

## 1. Key 分工

| 配置项 | 用途 | 可见范围 |
| --- | --- | --- |
| `TENCENT_MAP_JS_KEY` | JavaScript API GL 地图渲染 | 会在页面运行时对用户可见，必须限制来源和额度 |
| `TENCENT_MAP_SERVER_KEY` | 地理编码及驾车、步行、公交路线规划 WebService | 仅 Crow 服务端读取，不得发送给客户端或写入日志 |

两个 Key 不得混用。分别在腾讯位置服务控制台启用所需 API，并按控制台能力配置来源、出口 IP、配额和告警。

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
3. 启动用户端并登录，验证导航页优先显示腾讯路线来源、距离、预计时间、起终点标记和路线折线。
4. 临时断网或使用无效 Server Key，验证此时才显示 `LOCAL_FALLBACK`、浏览器导航入口和 Haversine 距离；使用无效 JS Key 时仍保留服务端路线摘要和浏览器导航。

## 4. 常见问题

| 现象 | 检查项 |
| --- | --- |
| 页面提示鉴权失败 | JS Key 是否启用 JavaScript API GL；允许来源是否与 `TENCENT_MAP_JS_ORIGIN` 完全一致 |
| 地理编码失败 | 是否误用了 JS Key；WebService API 是否启用；服务端出口限制是否正确 |
| 路线只显示本地降级 | Server Key 是否同时启用了驾车、步行和公交路线规划；配额、出口限制和服务端超时是否正常 |
| 有路线摘要但无内嵌地图 | JS Key 来源限制是否允许 `TENCENT_MAP_JS_ORIGIN`；Qt WebEngine 是否安装；系统是否支持 WebGL |
| 程序未读取配置 | 环境变量、`NCS_ENV_FILE` 和 `.env` 路径是否正确；进程是否在修改后重启 |
| Key 曾被提交 | 立即在控制台重新生成；删除文件或补充 `.gitignore` 不能消除 Git 历史泄露 |
