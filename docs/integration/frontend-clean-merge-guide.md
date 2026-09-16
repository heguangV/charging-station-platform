# #44 干净前端替代方案与负责人合并步骤

## 本次交付

来源：`cjyhjy:codex/frontend-go-clean`，目标：`heguangV:develop`。
基线为当前 develop `422ee83`；完整前端取自 `8a648a0`，已经包含 #44 的 Vue/Go 适配及 #47 的 HTTP/commandId 修复。
只迁入 `apps/user`、`apps/admin` 的 Vue 源码、测试、public 资源、npm 清单及 Vite/Vitest 配置，和 `apps/shared`；另加 Web CI、本说明和构建产物忽略规则。

这里“剔除 C++”指不把 #44 新增/修改/删除的 C++、CMake、旧数据库、旧服务端和关联脚本带入 develop。develop 原有历史文件不删除，仍可用于历史构建；本次不宣称完成全仓旧栈退役。
后端 Go 功能由 #45/#46 负责，本 PR 不覆盖 backend 或 API 契约。

## 负责人操作顺序

1. 审查并将 #46 合入 `codex/backend/b-08-admin-archive-batch`，重新检查 #45 的 CI/回归后合入 develop。
2. 对本替代 PR 执行 GitHub 的 **Update branch**（若需要且允许），或由分支作者将最新 develop 普通 merge 到此分支。不要 rebase/强推共享分支。若出现冲突，先解决并重新执行本说明的验证。
3. 检查本 PR 的 **Files changed**：不应出现 `.cpp`、`.h`、CMake、`agent/`、`core/`、`infrastructure/`、`server/`、`backend/` 或数据库迁移的改动。确认 Web CI 两个作业通过；原有必需 CI 及审批也须满足。
4. 验证 Go API 联调后，将此替代 PR 合入 develop。无需合入 #44 或 #47，也无需再 cherry-pick `8a648a0`，因为相关 Web 内容已包含。
5. 确认替代 PR 合并成功后，将 #44、#47 **Close pull request**（不是 Merge），注明被本干净前端 PR 替代。不要删除或强推原分支。#42 还有更广的历史迁移内容，应由负责人逐项确认，不能仅因本 PR 完成而认定它全部被替代。
6. 部署时先升级后端并执行其 0001..0012 迁移，再发布 Web。现有根 CMake 是历史栈入口；正式 Web 使用 npm 构建，Go 使用 backend 下的命令，不使用 CMake 构建 Web。

## 如何复现剔除

本次采用文件白名单，从干净 develop 提取最终 Web 快照，而非把 #44 的混合提交全部 cherry-pick。下面的操作只应在新建的隔离 worktree 中执行（示例目录/分支须未存在）：

```bash
git fetch origin develop
git fetch https://github.com/cjyhjy/charging-station-platform.git codex/frontend-go-followup
git worktree add -b codex/frontend-clean-rebuild ../frontend-clean-rebuild origin/develop
cd ../frontend-clean-rebuild
# 8a648a0 是含 #47 修复的已验证前端源提交。
git restore --source=8a648a0 -- \
  apps/user/src apps/user/tests apps/user/public \
  apps/user/index.html apps/user/package.json apps/user/package-lock.json \
  apps/user/vite.config.js apps/user/vitest.config.js \
  apps/admin/src apps/admin/tests apps/admin/public \
  apps/admin/index.html apps/admin/package.json apps/admin/package-lock.json \
  apps/admin/vite.config.js apps/admin/vitest.config.js apps/shared
```

这一步不删除原有 Qt 文件，也不携入旧后端变更。本替代 PR 还增加了 Web CI、dist 忽略规则和两处 Go 代理配置注释；复现时须一并核对。

## 构建、配置与验收

使用 Node 22+，在 `apps/user`、`apps/admin` 分别运行：

```bash
npm ci
npm run test
npm run build
```

在仓库根 `.env` 设置公开的 `VITE_NCS_API_TARGET=http://127.0.0.1:8080`（或实际 Go API 地址）；车主端需要底图时可配置受来源限制的 `TENCENT_MAP_JS_KEY`。服务端地图及模型密钥不得以 `VITE_` 命名。分别 `npm run dev`，车主端端口 5173，管理端 5174；生产由 Web 服务器将 `/api` 反代至 Go API。

本干净 worktree 全新 `npm ci` 后：user 122/122、admin 158/158 测试通过，两端 build 通过。测试包含加载、空数据、失败/重试恢复和 reduced-motion；此前相同 Web 源码已有真实 Go API 浏览器联调记录，见 #46 的验证报告。本次未重新声称做过新的供应商实网验收。
合并基线还应验收登录、站点查询、订单/充电启停、钱包、管理端设备命令，以及 API 失败后的重试恢复；后端闭环及数据库验证按 #46 的合并说明执行。

本次保持源分支 lockfile 不变。`npm audit` 发现已有依赖问题：user 共 5 项（生产依赖 0），admin 共 6 项（生产依赖 ECharts 1 项 moderate）；两端开发依赖中含 Vite high、Vitest critical。依赖安全验收尚未通过，发布前应按安全基线另行升级和回归，不能把测试通过写成安全问题已解决。管理端现有图表包也有大于 500 kB 的构建提示。

## 回滚

仅回退本前端 PR 的合并提交并重新部署上一版 Web，不需要回滚数据库。Go 后端的应用/迁移回滚按 #46 文档执行。若本 PR 仍未合并，可关闭它而不影响 develop；不对 #44 的共享历史执行 reset 或 force push。
