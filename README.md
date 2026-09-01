# NCS 电动汽车充电桩应用管理平台

NCS 是一个用于课程实训、团队研发和功能演示的充电桩管理平台。当前目标是在 Ubuntu 上使用 C++17 与 Qt Widgets 完成车主用户端和 PC 服务器管理端，使用 SQLite 保存业务数据，并扩展腾讯地图、Vue/ECharts 数据大屏和 Python 机器学习预测。

项目使用模拟充电桩、虚拟余额和模拟支付，不连接真实电力设备，也不处理真实资金。

## 当前状态

仓库目前处于需求冻结和工程重构前期：已有可构建的 Qt Widgets 学习原型及远期 QML 地图实验，完整用户端、管理端、共享 SQLite 数据层、Socket 服务、大屏和机器学习模块尚未实现。

需求文档描述的是最终交付目标，不代表对应功能已经完成。实现状态必须以代码、测试和验收记录为准。

## 文档导航

- [软件需求规格说明书](docs/01-requirements-specification.md)：最低功能与验收基线，开发时优先查阅对应 `UC-*`、`BR-*` 和 `NFR-*`。
- [需求分析](docs/requirements-analysis.md)：统一后的产品范围、业务规则、技术方案和实施阶段。
- [需求决策与未确定问题](docs/requirements-gap-analysis.md)：需求冲突、已冻结决定和暂缓问题。
- [腾讯地图接入与密钥管理](docs/tencent-map-setup.md)：本地 Key 配置、安全限制和验证方法。

## 仓库结构

```text
.
├── .codex/skills/                  项目专用 Codex Skill
├── apps/mobile/                    远期 Android/QML 地图实验，不纳入当前验收
├── docs/                           SRS、需求分析、决策记录和接入说明
├── src/                            当前 Qt Widgets 学习原型
├── .env.example                    本地配置示例，不包含真实密钥
├── CMakeLists.txt                  CMake 工程入口
├── LICENSE                         GPL-3.0 许可证
└── README.md                       仓库入口与开发说明
```

目标业务模块将按需求分析逐步拆分为用户端、管理端、公共领域/服务层、SQLite 基础设施、大屏、机器学习和测试目录。在目录真正建立前，README 不把规划路径描述为已有实现。

## 环境要求

当前桌面工程的最低开发环境如下：

| 项目 | 要求 |
| --- | --- |
| 操作系统 | Ubuntu 22.04+；Windows 10/11 保持源码构建能力 |
| 编译器 | GCC 11+ 或 MSVC 2019/2022 |
| C++ | C++17 |
| Qt | 最低 6.5.3，当前验证版本 6.8.3 |
| Qt Creator | 6.2+（可选） |
| 构建工具 | CMake 3.16+、Ninja |
| Qt 模块 | Widgets、Network、Sql；后续管理图表需要 Charts，地图实验需要 Quick 与 WebView |
| 数据库 | SQLite 3，通过 Qt QSQLITE 驱动访问 |

Web 大屏和机器学习模块开发时还需要 Node.js/Vue 3/ECharts 与 Python 3.10+/scikit-learn；这些模块尚未加入当前构建。

## 构建与运行

推荐使用 Qt 安装目录中的 `qt-cmake`：

```bash
/path/to/Qt/6.8.3/gcc_64/bin/qt-cmake -S . -B build -G Ninja
cmake --build build --target codex-qt-demo
./build/codex-qt-demo
```

无桌面环境可执行烟雾测试：

```bash
QT_QPA_PLATFORM=offscreen ./build/codex-qt-demo --smoke-test
```

`codex-qt-demo` 是当前学习原型目标，并非最终产品名称。业务重构后将分别建立用户端和管理端目标。

远期 QML 实验默认不参与构建；仅在安装 Qt Quick 与 Qt WebView 后显式启用：

```bash
/path/to/Qt/6.8.3/gcc_64/bin/qt-cmake \
  -S . -B build-mobile -G Ninja \
  -DNCS_BUILD_ANDROID_EXPERIMENT=ON
cmake --build build-mobile --target ncs-mobile
```

## 本地配置与密钥

需要腾讯地图实验时复制配置模板：

```bash
cp .env.example .env
```

只在本地 `.env` 中填写真实 Key。`.env`、数据库、日志、备份和构建产物均不得提交到 Git。客户端可见的腾讯地图 JS Key 必须限制来源和配额；WebService Key 只能由 PC 服务器端读取。详细方法见[腾讯地图接入与密钥管理](docs/tencent-map-setup.md)。

## 开发规范

- 需求以 SRS 为最低基线；新增设计不得删减既有功能、异常流或验收标准。
- C++ 源码使用 UTF-8 和 C++17，保持 Qt 6.5.3 兼容；Windows 编译使用 `/utf-8`。
- 单个手写源文件不超过 400 行。UI 层不写 SQL、计费、余额扣减或权限规则。
- 数据库查询必须参数化；跨表写操作使用事务、幂等键和必要的唯一约束。
- 路径通过 `QDir`、`QFileInfo` 或 `QStandardPaths` 构造，不硬编码盘符和平台分隔符。
- 数据库和耗时任务不得阻塞 UI 或服务器事件循环，后台线程不得直接操作界面对象。
- 错误必须提供明确提示；禁止把密码、验证码、令牌、完整手机号或真实密钥写入日志。
- 新功能应同时补充对应测试和文档，并在提交前完成构建、相关测试及 `git diff --check`。

## Git 与协作

- 从最新主分支创建短生命周期功能分支，建议使用 `feature/`、`fix/`、`docs/` 前缀。
- 每次提交只处理一个清晰目的，提交说明应描述行为变化和原因。
- Pull Request 应关联需求编号，说明验证方法，并避免混入无关格式化或生成文件。
- 不提交 `.env`、Qt Creator 用户配置、构建目录、数据库、日志、备份或个人密钥。
- 修改 SRS、需求分析或 Skill 时同步检查相互引用，避免出现冲突或失效链接。

## 许可证

本项目采用 [GNU General Public License v3.0](LICENSE)。
