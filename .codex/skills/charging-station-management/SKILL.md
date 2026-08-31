---
name: charging-station-management
description: 规划、实现和维护电动汽车充电桩应用管理平台 NCS，覆盖 Qt 用户端与管理端、SQLite 验收版，以及 Android、云后端、PostgreSQL、Web 大屏和机器学习扩展。适用于本仓库的需求、设计、编码、测试和文档；不用于无关 Qt 示例。
---

# NCS 充电桩平台开发

## 需求路由

开始工作前阅读仓库根目录的 [README](../../../README.md)。实现具体功能时读取 [SRS](../../../docs/01-requirements-specification.md) 中对应的 `UC-*`、`BR-*`、`NFR-*`；涉及范围或技术选择时读取 [差异与未确定问题](../../../docs/requirements-gap-analysis.md)。

SRS 文字需求是 A 线教学验收基线。原始项目要求书补充背景，参考图片不规定具体视觉。Android、网络后端和 PostgreSQL 属于 B 线扩展；除非明确批准替换，不得用扩展架构删除或改写 A 线验收行为。

## A 线验收基线

- `ncs_user`：Qt Widgets 手机式桌面用户端，420×760。
- `ncs_admin`：Qt Widgets + Qt Charts 管理端，最小 1280×800。
- 两个程序在单机访问同一 SQLite `charge_platform.db`，启用外键、WAL、迁移和参数化查询。
- Vue 3 + ECharts 大屏读取每 30 秒更新的 `dashboard.json`。
- Python 3.10 + RandomForestRegressor 训练、评估和回写预测。
- Windows/MSVC 可构建，Ubuntu 22.04/GCC 11 是最终验收环境。
- 腾讯地图有 Key 时提供内嵌路线，无 Key/网络时使用预置坐标、Haversine 与系统浏览器兜底。

## A 线关键行为

- 手机号验证码登录并自动注册，包含默认昵称/头像、冻结校验和资料持久化。
- 一个用户最多一个预约/充电中订单，一个电桩最多被一个订单占用。
- 余额至少达到起充门槛；故障设备不可预约；预约保留 15 分钟。
- 每秒模拟充电，默认时间倍率 60；进程重启后恢复进行中计费。
- 预约、结算和批量建桩使用事务；结算失败整体回滚。
- 管理端具备营收折线/柱状图、状态饼图、设备/站点/用户管理、模拟远程重启和预测页面。
- 种子数据至少 5 站，每站 6～12 桩且含故障设备，并有约 30 天历史订单。
- 大屏和 ML 的布局、特征、指标、模型路径与验收清单按 SRS 执行。

## B 线扩展边界

- Android 使用 Qt Quick/QML，包名 `io.github.heguangv.chargingstation`。
- C++/Qt 后端提供 HTTPS/WebSocket，PostgreSQL 部署在 Ubuntu Server 24.04，Nginx 终止 TLS。
- 即时排队只在没有空闲设备时使用；5 分钟确认不能取代 A 线的 15 分钟直接预约，除非需求明确修改。
- WebSocket/TPNS 只负责提醒，业务状态必须重新从后端读取。
- 用户名/手机号密码、细粒度角色、服务费、目标金额、赠送余额、备份和审计是扩展规则；不得悄悄改变 A 线数据与截图验收。
- 推荐共享领域类型和服务接口，为 SQLite 本地服务与 PostgreSQL 网络服务建立不同适配器；不要强行共享 Widgets 与 Android QML UI。

## 不变量与安全

- UI 不出现 SQL；所有输入使用参数化查询，跨表操作使用事务。
- 金额、电量、价格和时间的存储与显示精度必须明确；迁移到 B 线时保留兼容映射。
- 开始、结束、结算、充值、设备命令和网络重试必须避免重复生效。
- 订单、充电记录和余额流水不因账号删除而丢失；删除策略需要保留历史引用。
- 模拟验证码和 `admin/123456` 仅用于隔离 A 线演示，禁止部署到公网。
- 管理员密码保存加盐摘要；B 线使用专用密码哈希，不使用单次 SHA-256。
- 地图、推送、数据库和云服务密钥不进入源码、APK、Git 或日志。
- 异常退出、数据库损坏、网络/地图失败、Python 失败和空数据都有明确提示或降级，不静默失败。

## 工程边界

- `client_user`：A 线 Widgets 用户端。
- `client_admin`：A 线 Widgets 管理端。
- `core/domain`、`core/service`、`core/database`：共享领域、用例和 SQLite 服务。
- `db`：schema、迁移与种子数据。
- `web`：Vue/ECharts 与 JSON 数据契约。
- `ml`：训练、预测、评估、模型和特征代码。
- `apps/mobile`、`apps/server`、`infrastructure/postgres`：B 线扩展。
- `tests`：领域、数据库、并发、恢复、UI 烟雾与端到端测试。

## 完成标准

- 每项实现可追踪到需求编号、异常流和验收标准。
- Qt 6.8.3 当前环境可构建，并验证 Ubuntu 22.04；跨平台要求按 SRS 处理。
- SQLite 可从空库初始化且不重复播种；WAL、外键、事务和恢复行为有测试。
- 用户端、管理端、大屏和 ML 生成 SRS 验收所需截图或指标。
- 性能、窗口尺寸、400 行限制、日志格式、UTF-8 和参数化 SQL 满足 NFR。
- 交付说明明确区分已完成的 A 线、B 线扩展和仍未确认的差异。
