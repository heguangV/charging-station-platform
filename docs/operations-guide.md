# NCS 运行与运维手册

| 项目 | 内容 |
| --- | --- |
| 适用范围 | 开发、测试和验收环境的启动、检查、备份、恢复与清理 |
| 需求依据 | SRS `NFR-M-04`、`NFR-R-*`、`NFR-D-*` |
| 当前状态 | 阶段一基础能力；标注“阶段二/三”的命令需对应实现完成后启用 |
| 更新日期 | 2026-09-02 |

## 1. 环境与配置

要求 CMake 3.24+、Ninja、GCC 11+ 和 Qt 6.2.x。复制 `.env.example` 为本机 `.env`，设置文件权限为仅当前用户可读写。配置文件依次由命令行 `--config`、`NCS_ENV_FILE`、当前目录 `.env`、程序目录 `.env` 选择；进程环境变量再覆盖文件中的同名配置。

正式环境必须满足：`NCS_ENV=production`、`NCS_ALLOW_INSECURE_HTTP=false`，并配置可读的证书和私钥路径。日志和安全输出不得包含配置值中的密钥、令牌、验证码或个人信息。

## 2. 构建与启动顺序

```bash
export QT_CMAKE=/path/to/Qt/6.2.x/gcc_64/bin/qt-cmake
./scripts/configure.sh dev
./scripts/build.sh dev
./scripts/test.sh dev
./scripts/smoke-test.sh
```

完整运行时按以下顺序启动：

1. 启动 `ncs_server`，等待 `/api/v1/health/ready` 成功；健康接口在阶段三交付前不可作为现有能力。
2. 启动 `ncs_user` 和 `ncs_admin`，确认连接状态正常。
3. 需要时启动 Dashboard 与 ML；两者失败不得阻断基础充电结算。

阶段一可直接执行：

```bash
./build/dev/server/ncs_server --smoke-test
QT_QPA_PLATFORM=offscreen ./build/dev/apps/user/ncs_user --smoke-test
QT_QPA_PLATFORM=offscreen ./build/dev/apps/admin/ncs_admin --smoke-test
```

## 3. 运行检查

| 检查项 | 正常结果 | 异常处置 |
| --- | --- | --- |
| 进程存活 | `/health/live` 返回成功 | 检查进程退出码与脱敏应用日志 |
| 服务就绪 | `/health/ready` 返回成功 | 检查配置、数据库版本、锁和外部依赖状态 |
| 客户端连接 | 状态栏显示服务可用 | 校验主机、端口、证书信任和系统时间 |
| 日志 | `ncs_YYYYMMDD.log` 持续写入 | 检查目录存在、空间和写权限 |
| 数据目录 | 仅服务端账号可读写 | 立即收紧权限并排查客户端越界访问 |

应用日志保留 30 天，审计/运维日志保留 180 天。清理任务必须按记录类别执行，不得用应用日志清理逻辑删除审计证据。

## 4. 备份与恢复（阶段二/三）

- 备份只能经 SQLite Online Backup API 或设计文档允许的一致性机制生成，运行时禁止直接复制数据库文件。
- 每日快照保留 7 份、每周快照保留 4 份；备份记录写入 `backup_record`。
- 恢复验证必须在隔离目录和独立服务进程中完成，不覆盖正在运行的数据库。
- 每次验证记录备份编号、校验结果、schema 版本、耗时和操作者；目标为 RPO 24 小时、RTO 4 小时。

恢复流程：停止写入并保留现场 → 选择最近已验证备份 → 在隔离目录恢复并校验 → 切换服务端数据库路径 → 启动就绪检查 → 抽查活动流程、钱包和审计记录 → 记录恢复结论。任何一步失败都回到原数据库路径，不删除现场文件。

## 5. 故障处理

| 场景 | 第一响应 | 禁止事项 |
| --- | --- | --- |
| 数据库无法打开 | 停止写请求，保留错误与文件权限证据 | 反复重建或覆盖原库 |
| schema 版本不匹配 | 保持只读/未就绪，核对迁移清单 | 修改已执行迁移 |
| 锁超时 | 记录请求 ID，检查长事务与工作线程 | 在事件循环中重试 SQL |
| 结算中断 | 按幂等键和恢复检查点重放 | 人工直接改余额 |
| 证书失效 | 切换已审核证书并重启验证 | 临时开放公网明文 HTTP |
| 外部地图/ML 失败 | 启用本地距离或朴素预测降级 | 阻断基础充电流程 |

## 6. 数据清理

测试或开发运行数据只能显式执行：

```bash
./scripts/clean-runtime-data.sh test --yes
./scripts/clean-runtime-data.sh development --yes
```

脚本拒绝处理 production。清理不可从 Git 恢复；执行前确认数据库、日志和测试证据已按需要归档。开发证书使用 `./scripts/generate-dev-cert.sh` 生成，仅限回环或受控 VM，30 天后替换。

## 7. 运维记录

每次部署、备份验证、恢复、证书替换或数据清理至少记录：时间、环境、操作者、变更版本、请求/任务编号、执行命令、结果、异常、回滚和证据位置。记录中只写相对或受控标识，不写密钥与个人信息。
