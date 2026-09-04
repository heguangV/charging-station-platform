# 腾讯地图接入

本文只说明腾讯地图的本地配置、验证和排错。地图功能与降级行为以 SRS 的 `UC-U-02`～`UC-U-04` 为准，构建环境见仓库 README。

## 1. Key 分工

| 配置项 | 用途 | 可见范围 |
| --- | --- | --- |
| `TENCENT_MAP_JS_KEY` | JavaScript API GL 地图渲染 | 会在页面运行时对用户可见，必须限制来源和额度 |
| `TENCENT_MAP_SERVER_KEY` | 地理编码和路线规划 WebService | 仅 Crow 服务端读取，不得发送给客户端或写入日志 |

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

配置优先级从高到低为：命令行参数（`--tencent-map-key`）、进程环境变量（`NCS_TENCENT_MAP_KEY`）、`NCS_ENV_FILE` 指定文件、仓库或程序目录中的 `.env`。文件中除腾讯地图服务端 Key 使用 `TENCENT_MAP_SERVER_KEY` 外，其余条目与进程环境变量同名。`.env` 支持注释（`#`）、空行、`export` 前缀与引号包裹的值，同一键首次出现生效，空值视为未设置；`NCS_ENV_FILE` 指向的文件缺失或不可读时服务端拒绝启动。真实 Key 不得写入源码、普通日志或 Git。

## 3. 验证

1. 确认 `.env` 被 Git 忽略：`git check-ignore -v .env`。
2. 启动服务端，验证 WebService Key 可完成一次北京地址地理编码。
3. 启动用户端，验证地图页加载、北京演示站点标记和错误提示。
4. 临时断网或使用无效 Key，验证站点列表、预置坐标、Haversine 距离和浏览器导航降级仍可使用。

## 4. 常见问题

| 现象 | 检查项 |
| --- | --- |
| 页面提示鉴权失败 | JS Key 是否启用 JavaScript API GL；允许来源是否与 `TENCENT_MAP_JS_ORIGIN` 完全一致 |
| 地理编码失败 | 是否误用了 JS Key；WebService API 是否启用；服务端出口限制是否正确 |
| 程序未读取配置 | 环境变量、`NCS_ENV_FILE` 和 `.env` 路径是否正确；进程是否在修改后重启 |
| Key 曾被提交 | 立即在控制台重新生成；删除文件或补充 `.gitignore` 不能消除 Git 历史泄露 |
