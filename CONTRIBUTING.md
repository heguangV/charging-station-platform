# 参与 NCS 开发

## 开始前

1. 阅读与任务直接相关的 SRS 编号和设计文档。
2. 从最新 `develop` 创建 `feature/<账号>/<主题>`、`fix/...` 或 `docs/...` 分支。
3. 在 Issue 或任务中写明验收结果、影响的数据/接口及失败路径。

## 本地质量门禁

```bash
export QT_CMAKE=/path/to/Qt/6.2.x/gcc_64/bin/qt-cmake
./scripts/configure.sh dev
./scripts/build.sh dev
./scripts/test.sh dev
./scripts/smoke-test.sh
./scripts/check.sh
```

真实配置由 `.env.example` 复制到 `.env` 后填写。不得提交 `.env`、凭据、数据库、日志、备份和构建产物。

## 变更原则

- 需求变化先改 SRS；数据库结构和公开契约分别先改对应设计文档及测试。
- UI、Controller、应用服务、领域和基础设施保持单向依赖；客户端不得打开 SQLite。
- 提交应聚焦单一目的，建议使用 `feat:`、`fix:`、`docs:`、`test:`、`refactor:` 或 `chore:` 前缀。
- Pull Request 使用仓库模板，填写实际执行的验证命令及结果，不用计划中的测试代替证据。

完整分支和审核规则见[研发实施指南 §7](docs/development-guide.md#7-变更与协作)。
