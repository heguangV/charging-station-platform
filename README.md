# NCS 电动汽车充电桩应用管理平台

NCS 是一个基于 C++17、Qt 6.2 Widgets、Crow 和 SQLite 的充电桩管理教学项目，包含车主用户端、PC 管理端、HTTPS REST/WebSocket 业务服务端、数据大屏和机器学习预测。系统使用模拟充电桩、虚拟余额和模拟支付，不连接真实电力设备或真实资金渠道。

## 文档

- [软件需求规格说明书](docs/01-requirements-specification.md)：正式需求与最低验收基线。
- [需求与研发指南](docs/development-guide.md)：产品范围、业务规则、技术边界、研发流程和完成标准。
- [腾讯地图接入说明](docs/tencent-map-setup.md)：Key 配置、安全限制和验证方法。

## 仓库结构

```text
.
├── apps/mobile/                    远期 Android/QML 地图实验
├── docs/                           SRS、研发指南和专项说明
├── src/                            当前 Qt Widgets 原型
├── CMakeLists.txt                  CMake 工程入口
├── LICENSE                         GPL-3.0 许可证
└── README.md                       仓库入口
```

业务模块将按研发指南逐步拆分为用户端、管理端、Crow 服务端、公共领域/服务层、SQLite 基础设施、大屏、机器学习和测试目录。SQLite 只由服务端访问，客户端通过 `/api/v1` HTTPS REST 和鉴权 WebSocket 通信。

## 本地文件

项目专用 Codex Skill、`.env`/`.env.*` 环境配置以及图片文件仅保存在本地，均已通过 `.gitignore` 排除，不会提交到 GitHub。新环境需要自行创建 `.env` 并填写所需配置，真实密钥不得写入源码或提交记录。

## 环境要求

| 项目 | 要求 |
| --- | --- |
| 操作系统 | Ubuntu 22.04+；兼容 Windows 10/11 源码构建 |
| 编译器 | GCC 11+ 或 MSVC 2019/2022 |
| C++ | C++17 |
| Qt | 6.2 |
| Qt Creator | 6.2+（可选） |
| 构建工具 | CMake 3.24+、Ninja |
| Qt 模块 | Widgets、Network、Sql、Charts；地图视环境使用 WebEngine 或系统浏览器降级 |
| 服务与通信 | Crow；HTTPS REST `/api/v1`；WebSocket 实时事件 |
| 数据与扩展 | SQLite 3；大屏使用 Vue 3/ECharts；ML 使用 Python 3.10+/scikit-learn |

## 构建

```bash
/path/to/Qt/6.2.0/gcc_64/bin/qt-cmake -S . -B build -G Ninja
cmake --build build --target codex-qt-demo
./build/codex-qt-demo
```

无桌面环境可运行：

```bash
QT_QPA_PLATFORM=offscreen ./build/codex-qt-demo --smoke-test
```

## 开发规范

- 实现必须关联 SRS 中的 `UC-*`、`BR-*` 或 `NFR-*`，不得降低既有功能和验收标准。
- C++ 源码使用 UTF-8、C++17 并保持 Qt 6.2 兼容；单个手写源文件不超过 400 行。
- UI 不包含 SQL、权限、计费或钱包规则；SQLite 只由 Crow 服务端访问，查询参数化，跨表写操作使用事务和持久化幂等键。
- 路径使用 `QDir`、`QFileInfo` 或 `QStandardPaths`，不得硬编码盘符和平台分隔符。
- 数据库和耗时任务不得阻塞事件循环，后台线程不得直接操作 UI。
- 错误必须有明确提示；密码、验证码、令牌、完整手机号和真实密钥不得写入日志。
- Codex Skill、`.env`/`.env.*`、图片、数据库、日志、备份、构建产物和个人 IDE 配置不得提交；真实 Key 只保存在本地 `.env`。
- 功能提交应同时包含必要测试和文档；提交前运行构建、相关测试、烟雾测试及 `git diff --check`。
- 分支使用 `feature/`、`fix/`、`docs/` 前缀；Pull Request 应关联需求编号并说明验证方法。

## Git 分支与推送原则

研发过程必须遵循以下分支流转规则：

1. **禁止直接推送到 `main` 分支。** `main` 仅用于保存已经审核并通过测试的正式版本，不得作为日常开发分支。
2. 开始开发前，从团队约定的最新开发基线创建个人分支。功能开发使用 `feature/<姓名或账号>/<功能名称>`，缺陷修复和文档修改分别使用 `fix/<姓名或账号>/<问题名称>`、`docs/<姓名或账号>/<文档名称>`。
3. 开发提交只能推送到对应的个人分支，不得直接推送到 `develop` 或 `main`。
4. 开发完成后，创建以 `develop` 为目标分支的 Pull Request，并填写完整的 PR 文档。PR 至少应包含：需求或问题背景、主要变更、影响范围、验证步骤与结果、相关需求/任务编号，以及必要的截图或日志。
5. Pull Request 必须通过代码审核；审核意见处理完毕且获得批准后，方可合并到 `develop` 分支进行集成测试。
6. 合并到 `develop` 后执行功能、回归及必要的集成测试。测试不合格时，应在个人分支修复并重新走审核和测试流程。
7. 仅当测试结果合格并满足发布条件后，才能通过 Pull Request 将 `develop` 合并到 `main`，形成正式版本。禁止绕过审核或测试直接合并。

分支流转路径：

```text
个人 feature/fix/docs 分支
        ↓ 提交 PR、填写文档、代码审核
     develop 分支
        ↓ 功能测试、回归测试、发布确认
       main 分支（正式版本）
```

## 许可证

本项目采用 [GNU General Public License v3.0](LICENSE)。
