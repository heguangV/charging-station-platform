# NCS 电动汽车充电桩应用管理平台

NCS 是一个基于 C++17、Qt Widgets 和 SQLite 的充电桩管理教学项目，包含车主用户端、PC 服务器管理端、数据大屏和机器学习预测。系统使用模拟充电桩、虚拟余额和模拟支付，不连接真实电力设备或真实资金渠道。

## 文档

- [软件需求规格说明书](docs/01-requirements-specification.md)：正式需求与最低验收基线。
- [需求与研发指南](docs/development-guide.md)：产品范围、业务规则、技术边界、研发流程和完成标准。
- [腾讯地图接入说明](docs/tencent-map-setup.md)：Key 配置、安全限制和验证方法。

## 仓库结构

```text
.
├── .codex/skills/                  项目专用 Codex Skill
├── apps/mobile/                    远期 Android/QML 地图实验
├── docs/                           SRS、研发指南和专项说明
├── src/                            当前 Qt Widgets 原型
├── .env.example                    本地配置模板
├── CMakeLists.txt                  CMake 工程入口
├── LICENSE                         GPL-3.0 许可证
└── README.md                       仓库入口
```

业务模块将按研发指南逐步拆分为用户端、管理端、公共领域/服务层、SQLite 基础设施、大屏、机器学习和测试目录。

## 环境要求

| 项目 | 要求 |
| --- | --- |
| 操作系统 | Ubuntu 22.04+；兼容 Windows 10/11 源码构建 |
| 编译器 | GCC 11+ 或 MSVC 2019/2022 |
| C++ | C++17 |
| Qt | 最低 6.5.3，当前验证 6.8.3 |
| Qt Creator | 6.2+（可选） |
| 构建工具 | CMake 3.16+、Ninja |
| Qt 模块 | Widgets、Network、Sql；后续使用 Charts，地图实验使用 Quick 与 WebView |
| 数据与扩展 | SQLite 3；大屏使用 Vue 3/ECharts；ML 使用 Python 3.10+/scikit-learn |

## 构建

```bash
/path/to/Qt/6.8.3/gcc_64/bin/qt-cmake -S . -B build -G Ninja
cmake --build build --target codex-qt-demo
./build/codex-qt-demo
```

无桌面环境可运行：

```bash
QT_QPA_PLATFORM=offscreen ./build/codex-qt-demo --smoke-test
```

## 开发规范

- 实现必须关联 SRS 中的 `UC-*`、`BR-*` 或 `NFR-*`，不得降低既有功能和验收标准。
- C++ 源码使用 UTF-8、C++17 并保持 Qt 6.5.3 兼容；单个手写源文件不超过 400 行。
- UI 不包含 SQL、权限、计费或钱包规则；数据库查询参数化，跨表写操作使用事务和幂等键。
- 路径使用 `QDir`、`QFileInfo` 或 `QStandardPaths`，不得硬编码盘符和平台分隔符。
- 数据库和耗时任务不得阻塞事件循环，后台线程不得直接操作 UI。
- 错误必须有明确提示；密码、验证码、令牌、完整手机号和真实密钥不得写入日志。
- `.env`、数据库、日志、备份、构建产物和个人 IDE 配置不得提交；真实 Key 只保存在本地 `.env`。
- 功能提交应同时包含必要测试和文档；提交前运行构建、相关测试、烟雾测试及 `git diff --check`。
- 分支建议使用 `feature/`、`fix/`、`docs/` 前缀；Pull Request 应关联需求编号并说明验证方法。

## 许可证

本项目采用 [GNU General Public License v3.0](LICENSE)。
