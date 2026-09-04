# NCS 临时后端待办

> 状态：临时工作清单，不是需求基线，不替代 SRS、接口文档或数据库设计。
>
> 生成日期：2026-09-02
>
> 范围：Crow 业务服务端、用户业务所需 SQLite 持久化、面向各客户端的通信接入、运行时任务和测试。
>
> 明确排除：尚未进入对应里程碑的完整历史种子。（管理端备份底层实现已完成，见第 6.3 节完成证据。）

## 1. 当前判断

- 当前 CMake 已生成 `ncs_server` 和原型 `codex-qt-demo`，`ncs_user` 与 `ncs_admin` 目标尚未建立。
- `server/`、`core/`、结构化日志、腾讯地理编码和用户业务 SQLite 仓储已建立；内存仓储只用于快速测试。
- Crow HTTPS、系统健康接口、用户端 30 条、管理端 31 条、Dashboard 3 条、ML 内部 4 条 REST 路由及鉴权 WebSocket 实时服务已建立；前端 API 客户端尚未实现。
- 当前冻结接口共70项：用户端30项、管理端31项、Dashboard 3项、ML内部4项、系统2项。
- `apps/mobile/` 是远期 Android/QML 地图实验，不计为当前 Qt Widgets 用户端完成项。
- 完成状态只以代码、契约和自动化验证证据为准。

## 2. 已统一的文档决策

以下冲突已在 SRS、接口文档和数据库设计中统一，实现与测试必须遵循这些结论。

- [x] 活动流程的未完成/待恢复状态统一为 `10、20、30、40、50、80`；终态为 `60、70、90`。
- [x] 头像由客户端预检并上传，服务端复验、重新编码和保存；客户端不保存服务端路径、不直接写库。
- [x] 充电流程统一为“请求/排队 → 报价 → 确认 → 预约 → 开始充电”。
- [x] 新增电站时由服务端在一个事务内创建电站和初始电桩；价格由行政区价格版本管理，不通过电站修改接口改价。
- [x] Dashboard 使用独立登录和退出路由；Dashboard Token 仅允许大屏读取和对应 WebSocket。
- [x] 补充受权头像读取接口；密码注册、密码登录、凭据维护和会话管理已写入 SRS 范围。
- [x] 需求追踪矩阵的目录和研发阶段已与实施指南统一。

## 3. P0：服务端工程基础

- [x] 建立 `ncs_server` Crow 可执行目标及独立入口。
  - 需求：`NFR-C-01`、`NFR-C-03`、`NFR-D-01`、`NFR-D-02`；实施指南阶段 1。
  - 代码：`CMakeLists.txt`、`server/main.cpp`；Crow 1.3.3 与 standalone Asio 1.30.2 固定版本接入。
  - 验证（2026-09-02）：Ubuntu 22.04 / GCC 11.4 / CMake 3.22.1 / Qt 6.8.3 下配置成功；`cmake --build build-ncs --target ncs_server --parallel 2` 通过；进程在 `127.0.0.1:8080` 启动，HTTP 连通探测收到尚未定义路由的预期 `404`，SIGINT 后退出码为 0。
- [x] 建立 `server/controller`、`server/middleware`、`server/runtime`。
  - 需求：实施指南第 2、3 节及阶段 1；Controller 只负责协议与 DTO，Middleware 承载公共 HTTP 能力，Runtime 承载持续任务。
  - 代码：`server/CMakeLists.txt`、`server/controller/CMakeLists.txt`、`server/middleware/CMakeLists.txt`、`server/runtime/CMakeLists.txt`；三个分层分别暴露 `ncs::server_controller`、`ncs::server_middleware`、`ncs::server_runtime` 目标。
  - 验证（2026-09-02）：CMake 重新配置通过；`cmake --build build-ncs --target ncs_server codex-qt-demo --parallel 2` 通过；重构后的 `build-ncs/ncs_server` 正常启动，HTTP 连通探测收到尚未定义路由的预期 `404`，SIGINT 后退出码为 0。
- [x] 建立 `core/domain` 与 `core/application`，避免 Controller 承载业务规则。
  - 需求：实施指南第 2、3 节及阶段 1；领域层不依赖 Qt Widgets、Crow、SQLite 或外部地图，Controller 只经由应用层进入业务用例。
  - 代码：`core/CMakeLists.txt`、`core/domain/CMakeLists.txt`、`core/application/CMakeLists.txt`；定义 `ncs::core_domain` 与 `ncs::core_application`，依赖方向为 `controller/runtime -> application -> domain`。
  - 验证（2026-09-02）：CMake 重新配置与 `ncs_server`、`codex-qt-demo` 回归构建通过；CMake Graphviz 目标图确认 `ncs_server_controller -> ncs_core_application -> ncs_core_domain` 及 `ncs_server_runtime -> ncs_core_application`，无核心层反向依赖；服务端启动和 HTTP 连通回归通过，SIGINT 后退出码为 0。
- [x] 定义环境配置模型、默认开发配置及启动参数。
  - 需求：`UC-U-08`、`NFR-C-01`、`NFR-D-01`、`NFR-D-02` 及实施指南阶段 1；开发默认充电时间倍率为 60，服务仅监听明确数字 IP。
  - 代码：`server/runtime/server_config.h`、`server/runtime/server_config.cpp`、`server/main.cpp`；支持开发、测试、验收和生产环境，命令行优先于 `NCS_*` 进程环境变量，默认为 `127.0.0.1:8443`、2 个工作线程和时间倍率 60；TLS 证书和私钥配置由下一项接入。
  - 验证（2026-09-02）：`ncs_server_config` 共 1 个 CTest 目标通过，覆盖默认值、环境变量、命令行覆盖、帮助/版本、数值边界、未知/重复参数及非受控地址拒绝；真实进程以 `NCS_PORT=18080` 与 `--port 18081` 启动后仅监听 18081，验证了命令行优先级；非法监听地址退出码为 2。
- [x] 配置 HTTPS 证书、受控监听地址和 `/api/v1` 路由前缀。
  - 需求：`NFR-D-01`、`NFR-D-02` 及接口文档第 1.1 节；开发默认地址固定为 `https://127.0.0.1:8443/api/v1`，服务不接受通配或多播监听地址。
  - 代码：`CMakeLists.txt`启用 Crow/OpenSSL；`server/runtime/server_config.*` 提供证书与私钥配置、路径规范化和启动前文件检查；`server/controller/api_routes.*` 只允许将相对路径注册到 `/api/v1`；`server/main.cpp` 强制调用 `ssl_file`。
  - 验证（2026-09-02）：2 个 CTest 目标全部通过，覆盖 TLS 配置、缺失/同文件拒绝、Crow 最少 2 线程边界及 `/api/v1` 路由约束；临时自签名证书下服务在 18443 端口启动，OpenSSL 确认 TLS 1.3 / `TLS_AES_256_GCM_SHA384`，HTTPS 连通成功且明文 HTTP 无法获得响应；无证书启动退出码为 2，临时证书和私钥已删除。
- [x] 实现启动检查、信号处理和优雅退出。
  - 需求：`NFR-C-04`、`NFR-R-02`、`NFR-D-01` 及实施指南阶段 1；无效启动条件必须给出明确错误且不进入 Crow 事件循环。
  - 代码：`server/runtime/startup_checks.h`、`server/runtime/startup_checks.cpp` 检查 PEM 大小/格式、证书有效期、私钥匹配、SAN 监听 IP、Unix 私钥权限和端口可绑定性；`server/main.cpp` 显式只注册 `SIGINT` 与 `SIGTERM`。
  - 验证（2026-09-02）：2 个 CTest 目标全部通过；真实启动分别覆盖证书/私钥不匹配、SAN 不匹配、私钥权限过宽和端口已占用，均在监听前以退出码 2 拒绝；`SIGTERM` 与 `SIGINT` 均关闭 acceptor 和 I/O 循环并以退出码 0 结束，退出后端口可立即重用；临时证书与私钥已删除。
- [x] 实现结构化日志、日志级别、模块名和请求 ID。
  - 需求：`NFR-M-04`、接口文档第 1.2、11 节及实施指南阶段 1；应用日志写入 `logs/ncs_YYYYMMDD.log`，包含级别、时间、模块和请求 ID 并保留 30 天。
  - 代码：`infrastructure/files/structured_logger.*` 实现线程安全 JSONL 日志、UTC 日切换、30 天清理、Unix `0600` 权限和线程局部请求作用域；`server/middleware/crow_log_handler.*` 接管 Crow 日志并过滤查询串与 Bearer 值；`server/runtime/server_config.*` 增加日志级别/目录配置；`server/main.cpp` 记录启动检查、Crow 生命周期和退出。HTTP `X-Request-ID` 生成与回传仍按第 4 节独立待办处理。
  - 验证（2026-09-02）：3 个 CTest 目标全部通过；日志测试覆盖 JSON 字段、级别过滤、请求 ID 作用域、过期清理、4 线程并发写入及 Crow 脱敏；真实 HTTPS 进程生成 `ncs_20260902.log`，权限为 `0600`，20 行均为 JSON，含完整启停事件，查询串被替换为 `?<redacted>` 且未出现测试 Token；临时日志、证书和私钥已删除。
- [x] 实现全局异常处理中间件，不向客户端泄露堆栈、SQL或内部路径。
  - 需求：`UC-U-01/E5`、接口文档第 1.3、1.10、14 节；未处理异常固定映射为 HTTP 500 / `INTERNAL_ERROR`（`code=13`），公开响应不得包含 SQL、路径、堆栈、令牌等内部信息。
  - 代码：`server/middleware/global_exception_handler.*` 安装 Crow 全局异常回调，覆盖可能存在的半成品响应，只返回六字段失败结构并禁止缓存；异常日志仅写通用事件和当前请求 ID，不读取或记录异常原文；`server/main.cpp` 在注册 API 路由前启用处理器。完整 `X-Request-ID` HTTP 中间件及通用响应封装仍按第 4 节独立待办处理。
  - 验证（2026-09-02）：4 个 CTest 目标全部通过；异常测试通过真实 Crow 路由抛出含 SQL、内部路径和令牌样例的异常，确认响应为 HTTP 500、`code=13`、UTF-8 JSON、`Cache-Control: no-store`，字段严格限定为失败契约六项，响应和结构化日志均不包含异常原文，且请求 ID 作用域保持一致；`ncs_server` 与 `codex-qt-demo` 回归构建通过。
- [x] 实现 `GET /api/v1/system/health/live`。
  - 代码：`server/controller/health_routes.*`；无需鉴权，只返回进程 `UP`，不访问数据库或暴露版本、路径。
- [x] 实现 `GET /api/v1/system/health/ready` 的协议层和安全响应。
  - 代码：`core/application/readiness_probe.h`、`server/controller/health_routes.*`、`infrastructure/sqlite/sqlite_repository.*`；只允许回环地址或具备 `OPERATOR/OWNER` 角色的管理员，响应仅含 schema、读写、WAL、迁移四个布尔检查。正式服务每分钟在后台刷新 SQLite 检查缓存，健康路由本身不阻塞 Crow 事件线程；任一检查失败返回 HTTP 503 / `DOWN`。
- [x] 增加服务端构建测试与无交互烟雾测试。
  - 验证（2026-09-02）：`ncs_server_build` 通过；`server_smoke_test.py` 自动生成临时证书、启动真实 HTTPS 服务、检查 live/ready、请求 ID、HSTS、CORS 拒绝和 SIGTERM 退出，临时证书、私钥及日志随隔离目录自动删除。

## 4. P0：公共 HTTP、鉴权与安全能力

- [x] 实现统一成功/失败响应封装。
- [x] 实现稳定 `ErrorCode` 与 HTTP 状态映射。
- [x] 实现 JSON 字段严格白名单和字段边界校验。
- [x] 实现 Bearer Token 解析、摘要校验和权限上下文。
- [x] 隔离普通用户、运营管理员、决策查看者和 ML 任务令牌。
- [x] 实现用户会话期限、最多三个终端及撤销机制。
- [x] 实现管理员会话期限、最多两个终端及撤销机制。
- [x] 实现管理员敏感操作15分钟重新验证窗口。
- [x] 实现密码专用哈希；默认演示密码只允许开发配置。
- [x] 实现模拟验证码生成、10分钟有效期、最多5次错误和60秒冷却。
- [x] 确保验证码仅在受限开发模式返回且从所有日志中过滤。
- [x] 实现 `X-Request-ID` 接收、生成、回传及日志贯穿。
- [x] 实现写接口 `Idempotency-Key` 校验、结果重放及冲突检测。
- [x] 实现资源 `version` 乐观锁和 `VERSION_CONFLICT`。
- [x] 实现分页上限、过滤参数和排序白名单。
- [x] 实现 JSON、预测批量和头像上传大小限制。
- [x] 实现请求超时、接口限流和 `Retry-After` 语义。
- [x] 实现最小化 CORS 策略和 TLS 安全配置。
- [x] 实现手机号、令牌、验证码、钱包和订单信息的日志脱敏。

公共 HTTP 模块完成证据（2026-09-02）：

- 需求：`UC-U-01`、`UC-A-01`、`UC-W-04`、`NFR-P-05`、`NFR-S-01`～`NFR-S-05`、`NFR-D-01`，以及接口文档第 1、12、14 节。
- 协议与校验：`core/domain/error_code.h`、`server/controller/api_response.*`、`request_validation.*` 实现冻结错误码、六字段响应、公开诊断脱敏、JSON 白名单/类型边界、分页 1～100、过滤/排序白名单、UUID 幂等键和乐观版本检查。
- 安全与状态：`core/application/security_crypto.*`、`session_manager.*`、`verification_code_service.*`、`idempotency_service.*` 实现 PBKDF2-HMAC-SHA256 版本化密码摘要、256 bit 随机 Token 及仅摘要存储、令牌类型/角色隔离、用户 30 天/3 终端、管理员 2 终端、Dashboard 8 小时及 30 分钟空闲、15 分钟重新验证、验证码冷却/有效期/错误锁定，以及并发幂等占位、冲突和结果重放。通用记录保留策略为至少 7 天，充值/结算永久保留；正式服务已将幂等记录接入 SQLite。
- Crow 接入：`server/middleware/request_policy_middleware.*`、`authorization.*` 在真实请求链路生成或校验 `X-Request-ID`，贯穿响应和日志；拒绝 URL Token、未知 CORS 来源和超限请求体；普通/统计截止时间为 10/30 秒，默认令牌桶为持续 20 请求/秒、50 请求突发，超限返回 HTTP 429、`RATE_LIMITED`、`Retry-After`；JSON/ML 批量/头像上限为 1/8/5 MiB。
- TLS 与日志：`server/runtime/startup_checks.*` 建立最低 TLS 1.2、限定 AEAD 密码套件、禁用旧协议的服务端上下文；`structured_logger.*` 对手机号、Bearer/Token、验证码、密码、钱包、余额和订单字段执行最终写入前脱敏。
- 验证：9 个 CTest 目标全部通过，覆盖配置、路由、异常、日志、安全服务、幂等/版本、公共 HTTP 契约、服务端构建和真实 HTTPS 烟雾；`ncs_server` 与 `codex-qt-demo` 构建通过。测试验证了响应白名单、错误映射、角色越权、会话数量/撤销/期限、重新验证、密码校验、验证码冷却/过期/五次锁定、生产模式不返回验证码、幂等并发/冲突/重放/保留、分页/过滤/排序、大小/超时/限流/CORS、live/ready 访问控制及敏感日志过滤。

HTTP 边界与运行时生命周期加固证据（2026-09-03）：

- 背景：复审确认“服务端基础与公共 HTTP”存在发布阻断项——Crow 先完整累积请求体导致资源耗尽、密码哈希与图片处理同步阻塞事件线程、会话/验证码/限流器无界增长、幂等异常后租约卡死、HEAD/OPTIONS 被错误改写为 503。相关“请求体限制、超时、CORS、运行时清理”完成项已重开并按本模块整改后重新验收。
- 框架级请求体上限：`cmake/patches/crow-http-body-limit.patch` 经 `cmake/apply_crow_patch.cmake` 固定应用，在解析 Content-Length 时即拒绝超大声明，chunked body 逐块累计超过上限立即终止并返回 413；`CMakeLists.txt` 定义 `CROW_HTTP_MAX_BODY_SIZE=8388608`；路由级 1/5/8 MiB 限制仍由 `server/middleware/request_policy_middleware.*` 执行；反向代理层限制属于部署文档职责，不作为唯一防线。
- HEAD/OPTIONS 与 CORS：`RequestPolicyMiddleware::context` 增加 `initialized` 标志，Crow 预路由路径上未初始化的上下文会生成请求 ID、补齐安全头与访问日志，且不执行虚假超时判断；`server/runtime/server_config.*` 新增 `--cors-allowed-origins` / `NCS_CORS_ALLOWED_ORIGINS`，来源白名单经启动参数传入中间件，默认空名单保持拒绝。
- 阻塞任务隔离：`core/application/bounded_executor.*` 提供有界工作池（默认 2 线程、64 队列），`server/controller/async_response.*` 将注册、密码登录、凭据修改、头像解码与 PNG 重编码、注销统一异步派发到工作池；队列满返回 429/`RATE_LIMITED` 并附 `Retry-After`，排队与执行共用按路径 10/30 秒的总截止时间预算。
- 限流与运行态回收：令牌桶改为 O(1) LRU，硬容量 4096、空闲 5 分钟清理，IPv6 客户端按 /64 聚合；密码登录独立限流（每秒 0.2、突发 5）；`core/application/session_manager.*` 定期清理过期/撤销会话并同步维护 token 摘要索引与按主体索引，登录、撤销、会话查询不再扫描全表，全局硬容量 16384；`verification_code_service.*` 清理过期/已消费验证码，每手机号每日上限 20 条，验证码与日签发表全局容量 10000/65536；`idempotency_service.*` 完成结果保留 7 天（永久项除外），InProgress 单独使用 10 分钟租约，超期可被原请求安全接管，容量 65536；`server/main.cpp` 每分钟统一执行会话、验证码、幂等、限流桶与日志清理。
- 幂等异常与崩溃安全：`IdempotencyLease` RAII 守卫在异常路径自动 abort；`executeAndComplete` 以 SQLite 外层事务包住业务写入和幂等结果落库，避免进程在两次提交之间退出后重复充值或结算；幂等查询、占位和事务执行均位于有界工作池，不阻塞 Crow 事件线程。
- 密码哈希参数：PBKDF2-HMAC-SHA256 迭代数提升至 600000（OWASP 当前建议），`loginPassword` 对低于当前迭代数的旧摘要执行透明重哈希，仓储通过 `replacePasswordHash` 以摘要 CAS 写回且不提升资源版本；Argon2id 迁移任务按 SRS `NFR-S-01` 继续保留。
- 验证（2026-09-03）：10 个 CTest 目标全部通过；`ncs_server_smoke` 以真实 HTTPS 覆盖未注册 HEAD（404 + `X-Request-ID`）、允许来源的 OPTIONS 预检（204 + CORS 头 + `X-Request-ID`）、拒绝来源 403、仅发送 `Content-Length=8MiB+1` 请求头即在收到 body 前获得 413、chunked 无总长发送超过 8 MiB 时在累积阶段被拒绝（`HTTP/1.1 413` + `X-Request-ID`）；`ncs_security_services` 覆盖 600k 摘要格式、旧迭代数验证、失败登录不重哈希、成功登录透明重哈希，以及按主体索引下 100 主体×3 终端限额、其他会话撤销隔离与过期清理；`ncs_idempotency_service` 覆盖守卫完成、异常中止、移动语义与租约过期回退；`ncs_structured_logger` 修复路径脱敏误吞 `?<redacted>` 查询标记的回归；`git diff --check` 通过。

用户身份模块复审整改证据（2026-09-03）：

- 背景：第二轮复审确认认证/会话/头像/注销 5 项声明与实现相符，同时指出 F1 验证码接口手机号枚举（中危）、F2 头像重编码体积膨胀（中危）、F3 纯数字用户名登录混淆（中低危）及若干低危备注。按“需求变化先修改文档”原则，先更新 `docs/database-api.md` 再改代码与测试。
- F1 防枚举：`core/application/user_identity_service.cpp` 的 `issueCode` 移除 RESET_PASSWORD 的账号存在性探测，所有手机号统一按冷却、每日上限和容量规则返回相同结构；`docs/database-api.md` 2.1 同步改为“响应内容与手机号是否已注册无关”。测试断言未知与已注册手机号的 RESET_PASSWORD 响应均为 200 且同结构。
- F2 头像资源边界：`server/controller/user_identity_dto.cpp` 将存储图降采样至不超过 1024×1024（等比、平滑变换，绘制时直接缩放避免二次大缓冲），归一化 PNG 结果超过 2 MiB 拒绝（422）；单请求解码峰值由约 192 MiB 降至约 70 MiB，单账号常驻存储不超过约 2 MiB。测试覆盖 2048×2048 渐变图降采样存储与 1024×1024 不可压缩图拒绝。
- F3 登录名混淆：`validUsername` 拒绝纯数字用户名，杜绝“用户名等于他人手机号”造成 `findByLoginName` 双匹配歧义与注册锁定骚扰；`docs/database-api.md` 2.2 同步约束。测试断言纯数字用户名注册返回 422。
- F4 令牌类型隔离：`/user/auth/logout` 只撤销 User 类型会话，与接口文档“用户、管理员、决策查看者和 ML 任务令牌互不通用”一致；测试断言管理员令牌调用用户 logout 后仍有效。
- F8 multipart 兼容性：boundary 解析改为标准参数扫描（任意参数位置、大小写不敏感、支持引号与 `; charset=utf-8` 变体），白名单与长度校验保持 fail-closed；测试以带尾随参数的 Content-Type 上传成功。
- 低危加固：昵称拒绝 C0/C1 控制字符（含 `\n`、`\u0080` 区段）。
- 知情保留（不改，需后续决策）：F5 凭据修改无乐观版本控制——新增字段属冻结接口变更，须先修订接口文档并与客户端同步；F6 注册在验证码校验前返回 AlreadyExists——常见 UX 权衡，保留现状；F7 内存仓储线性扫描与注销后 `balanceCent`/`debtCent` 留存——随 SQLite 持久化模块按财务留存要求重新设计。
- 验证（2026-09-03）：10 个 CTest 目标全部通过；`ncs_user_identity_routes` 新增 9 项断言全部通过（防枚举响应一致性、纯数字用户名、昵称控制字符、boundary 变体、降采样存储、1024 上限、不可压缩拒绝、管理员令牌 logout 隔离）；`git diff --check` 通过。另：第二轮复审文本中“第一轮 H1/M1/M2/M3/M4 一行未改”与当前代码不符，框架请求体上限、阻塞任务池、限流回收、幂等租约和 HEAD/OPTIONS/CORS 修复均已落盘并有真实 HTTPS 烟雾测试覆盖，见上一节证据。

## 5. P0：用户端 REST 接口（当前文档30项）

### 5.1 认证、会话与资料（13项）

- [x] `POST /api/v1/user/auth/sms/code`
- [x] `POST /api/v1/user/auth/register`
- [x] `POST /api/v1/user/auth/login/password`
- [x] `POST /api/v1/user/auth/login/sms`
- [x] `POST /api/v1/user/auth/logout`
- [x] `GET /api/v1/user/sessions`
- [x] `DELETE /api/v1/user/sessions/{sessionId}`
- [x] `GET /api/v1/user/me`
- [x] `PUT /api/v1/user/me`
- [x] `POST /api/v1/user/me/avatar`
- [x] `GET /api/v1/user/me/avatar/content`
- [x] `PUT /api/v1/user/me/credential`
- [x] `DELETE /api/v1/user/me`

认证、会话与资料模块完成证据（2026-09-03）：

- 需求：`UC-U-01`、`UC-U-05`、`NFR-S-01`～`NFR-S-04`，以及接口文档第 2 节；完整手机号、密码摘要、Token、验证码和服务端存储位置均不得进入公开资料响应。
- 应用与仓储：`core/application/user_account_repository.h` 定义账户持久化边界；`user_identity_service.*` 实现注册、短信自动注册、统一密码登录失败、冻结拦截、资料版本更新、凭据验证、其他会话撤销、活动流程注销拦截和匿名化；正式进程接入 `infrastructure/sqlite/sqlite_repository.*`，内存适配器仅用于快速测试。
- HTTP 与头像：`server/controller/user_identity_routes.*` 注册第 2 节全部 13 条路由并使用严格字段白名单；`user_identity_dto.*` 只返回脱敏手机号，头像按真实 PNG/JPEG/BMP 内容解码，限制为 4096×4096，绘制到无元数据图像后统一重编码为 PNG，并提供私有鉴权读取、`ETag` 和条件请求；注销后头像立即不可访问，满足 24 小时内删除上限。
- 会话：`session_manager.*` 增加当前用户有效会话查询、仅撤销本人指定会话、凭据修改后撤销其他会话及注销后撤销全部会话；重复退出和重复撤销保持幂等，不允许使用其他用户的会话标识越权撤销。
- 验证：`ncs_user_identity_routes` 覆盖 13 条路由、注册、密码与短信登录、手机号脱敏、未知字段、昵称版本冲突、会话查询/撤销、伪造头像拒绝、元数据剥离、PNG/ETag/304、凭据更新、其他会话撤销、重复退出、冻结登录拒绝、活动流程注销拦截、注销匿名化及令牌立即失效；`ncs_server_smoke` 通过真实 HTTPS 完成验证码→注册→资料→退出链路。

充电域复审整改证据（2026-09-03）：

- R1（中危）：`charge_flow_service.start` 的余额下限改为 `max(500 分, 客户端 balanceFloorCent)`，客户端传入 0 或任何低于全市统一最低起充金额的值不再削弱 BR-04 检查；回归测试覆盖"客户下限高于余额拒绝"与"0 下限保持城市下限"两条路径。
- R2（中危）：`wallet_service.recharge` 的账户钱包镜像写回移入仓储事务内，与结算路径一致，消除并发充值下镜像以旧快照收尾的窗口。
- R4（低危）：地理编码出站请求已改用 `QUrlQuery` 对关键字与 Key 编码，并补充超时中止；无 SSRF 面（主机固定）。
- R7（低危）：结算的 50→60 中间态不再单独递增版本，客户端观察到单次版本步进。
- 知情保留：R3 失败结果幂等重放（符合接口文档 1.8 字面语义，需与客户端约定重试策略）、R5 已取消/过期订单无小票（待产品确认）、R6 `targetAmountCent` 自动停充（待需求澄清）、R8/R9 随 SQLite 持久化模块收敛。
- 验证：14 个 CTest 目标全部通过（含并行的 `ncs_sqlite_repository` 与 `ncs_station_service` 目标）；`git diff --check` 通过。

钱包、站点、充电流程与运行时模块完成证据（2026-09-03）：

- 需求：`UC-U-02`～`UC-U-10`、`BR-02`～`BR-12`、`NFR-R-01`，以及接口文档第 3、4、5 节与数据库设计第 3.2、3.3、6 节。
- 5.2 钱包与订单：`core/application/wallet_service.*` 实现钱包概览、虚拟充值（1～1,000,000 分校验、先清欠费后入账、钱包版本递增）与流水分页/类型过滤；订单列表与小票由 `charge_flow_service` 提供，小票字段与 §5.8 `SettlementReceipt` 一致且仅属主可见。
- 5.3 站点设备价格：SQLite 迁移内置三个演示电站（含故障桩）与区域价格版本；`station_service.*` 实现地址定位（成功后不误作站名过滤）、`locationFallback`、Haversine 距离、类型过滤、距离排序、详情、设备列表和展示型报价。
- 5.4 充电流程：`charge_flow_service.*` 实现完整状态机（10 排队、20 待报价、30 已预约、40 充电中、50 结算中、60/70/80/90 终态），报价 5 分钟、预约 15 分钟、最低起充 500 分（BR-04）、指定设备并发冲突返回 `ALLOCATION_CONFLICT`、金额=电量×（电费+服务费）按分取整（BR-05）、余额不足部分转为欠费（BR-06）。
- 5.5 运行时：按"站点+类型"持久化 FIFO 队列与取消/过期后自动递补；每 15 秒维护任务处理报价和预约到期；开始时再次复验账号冻结、钱包和设备状态；计费倍率在开始充电时快照；启动时执行恢复和到期维护；结算事务失败整体回滚后单独转入 80，可按新版本重试。
- 幂等接线：`server/controller/idempotent_response.*` 将 `Idempotency-Key` 契约接入充值（永久保留）、创建流程、确认报价、取消、开始（7 天保留）与结算（永久保留），重放返回首次结果、同键不同体返回 `IDEMPOTENCY_CONFLICT`、无键返回 400；RAII lease 保证异常路径自动 abort，正式 SQLite 接线将业务操作与幂等完成记录置于同一事务。
- 结算通知：流程事件同步写入 `outbox_event`；结算成功的 `order.settled` 与订单、钱包、设备累计值及流程终态同事务提交，失败回滚不会留下伪成功通知。
- 地图：`infrastructure/map/tencent_geocoder.*` 通过 `NCS_TENCENT_MAP_KEY`/`--tencent-map-key` 配置服务端 Key（环境文件中写作 `TENCENT_MAP_SERVER_KEY`，见 `docs/tencent-map-setup.md`），调用失败、超时或无 Key 一律回退预置坐标与 Haversine 距离。
- 验证（2026-09-03）：14 个 CTest 目标全部通过。`ncs_sqlite_repository` 覆盖空库迁移、WAL、业务写与幂等完成的原子回滚、仓储实例重建后的充电恢复、结算失败转 80 与重试、outbox 原子提交及再次重建后的订单小票；`ncs_station_service` 覆盖地理编码成功和失败降级；真实 HTTPS 烟雾测试会重启 `ncs_server`，重新登录核验钱包与订单，并以相同充值幂等键确认余额不会重复增加。地图真实腾讯响应仍需要配置有效 Key 后执行外部集成验证。

### 5.2 钱包与订单（5项）

- [x] `GET /api/v1/user/wallet`
- [x] `POST /api/v1/user/wallet/recharges`
- [x] `GET /api/v1/user/wallet/transactions`
- [x] `GET /api/v1/user/orders`
- [x] `GET /api/v1/user/orders/{orderNo}`

### 5.3 电站、设备与价格（4项）

- [x] `GET /api/v1/user/stations`
- [x] `GET /api/v1/user/stations/{stationId}`
- [x] `GET /api/v1/user/stations/{stationId}/chargers`
- [x] `GET /api/v1/user/stations/{stationId}/quote`
- [x] 接入腾讯地图地理编码；失败时使用预置位置和 Haversine 降级。

### 5.4 充电流程（8项）

- [x] `POST /api/v1/user/flows`
- [x] `GET /api/v1/user/flows/active`
- [x] `GET /api/v1/user/flows/{flowNo}`
- [x] `POST /api/v1/user/flows/{flowNo}/quote-confirmations`
- [x] `POST /api/v1/user/flows/{flowNo}/cancellations`
- [x] `POST /api/v1/user/flows/{flowNo}/start`
- [x] `GET /api/v1/user/flows/{flowNo}/progress`
- [x] `POST /api/v1/user/flows/{flowNo}/settlements`

### 5.5 用户业务运行时

- [x] 实现用户活动流程唯一性检查。
- [x] 实现设备互斥分配和冲突后的安全业务响应。
- [x] 实现冻结、欠费、最低余额和设备可用性检查。
- [x] 实现按“站点+类型”的 FIFO 排队和自动递补。
- [x] 实现5分钟报价到期处理。
- [x] 实现15分钟预约到期、设备释放和队列递补。
- [x] 实现用户取消及其幂等结果重放。
- [x] 实现服务端持续计费和 `charge.time_scale` 快照。
- [x] 实现时长、电量、费用和模拟 SoC 计算。
- [x] 实现服务重启后的充电流程恢复。
- [x] 实现结束充电、结算失败恢复和幂等重试的应用逻辑。
- [x] 实现注销后的会话撤销、数据匿名化和头像延迟清理任务。

管理端 REST 与运行时模块完成证据（2026-09-03）：

- 需求：`UC-A-01`、`UC-A-04`～`UC-A-07`、`BR-07`、`BR-10`、`BR-11`、`NFR-S-04`，以及接口文档第 6～9 节。与并行开发的管理端骨架（认证服务、用户服务、审计事件、登录锁定）合并完成。
- 认证与用户管理（6.1～6.7）：`admin_auth_service` 实现登录（模拟哈希防时序探测、连续 5 次失败锁定 30 秒、尝试状态有界清理）、15 分钟重新验证与管理员退出；`admin_user_service` 实现用户列表（精确手机号/后四位/状态过滤、排序白名单）、详情（脱敏手机号 + 会话数 + 活动流程摘要）、冻结/解冻（乐观版本、冻结撤销会话但保留进行中充电、幂等键必填、写审计）与用户订单查询（访问写审计）。
- 站点设备价格（6.2 全部 13 项）：`admin_station_service` 实现站点新增（站点 + 初始设备同事务、按站点编码生成不重复设备编号、adcode 必须存在生效价格版本）、修改（禁止携带 initialCharger、adcode 变化需可解析价格）、停用/启用（活动流程拦截，BR-10）、设备批量创建（≤100、编码唯一、整批回滚）、设备状态变更（活动设备必须走受控释放或重启，BR-11）、远程重启命令（二次确认 + 重新验证 + 幂等键；待确认/已预约先强制释放、充电中先幂等受控结算，命令 2 秒模拟执行并回写设备空闲）、价格版本（同行政区区间不得重叠）与服务费调整（-2000～2000 且 500 步长，最终服务费钳制在基础服务费 80%～140%）。
- 流程统计审计备份（6.3 全部 8 项）：`admin_ops_service` 实现活动流程列表（status/stationId/chargerId/userId 过滤）、强制释放（仅 20/30，充电中返回 `INVALID_STATE_TRANSITION`，设备按管理员选择置位并递补队列，写审计）、营收统计（day/hour 分桶、范围上限 90 天）、设备状态统计（五状态计数 + 健康度）、审计查询（仅 OWNER、只读）、SQLite Online Backup 一致性快照及隔离副本恢复验证、NFR-R-03 备份保留（每日 7 份 + 每周 4 份轮换，失败诊断 7 天后清理）。
- 预测接口（6.4 全部 3 项）：`GET /predictions` 在 ML 子进程接入前如实返回空集合；`POST /ml-tasks` 相同类型运行中任务去重返回现有任务，PREDICT 校验 horizon ∈ {1,6,24}；`GET /ml-tasks/{taskNo}` 返回任务状态。任务按 UC-M-04 超时（训练 10 分钟、预测 2 分钟）转入 `TIMED_OUT`，不会永久占用运行中任务去重；超时由 15 秒维护循环与任务查询共同触发。
- 仓储扩展：`SqliteRepository` 通过追加迁移持久化管理员、角色、审计事件、价格调整、设备命令、ML 任务与备份记录；`InMemoryAdminRepository` 仅供隔离测试。正式服务的业务写、审计和幂等结果复用同一 SQLite 事务。
- 价格联动：`pricing.computePrice` 增加 `approvedAdjustmentBp`，排队压力与管理员批准调整合并钳制后作用于基础服务费；`StationService` 与 `ChargeFlowService` 通过注入的调整查询生成报价快照。
- 验证（2026-09-03）：15 个 CTest 目标全部通过；`ncs_admin_routes` 覆盖登录/错误密码/锁定、用户列表脱敏、冻结撤销会话且保留活动充电、版本冲突、批量编码唯一、活动设备状态保护、价格区间冲突、强制释放、重启命令、统计、审计、备份和 ML 任务去重（管理端全部写接口强制幂等键，符合接口文档 §1.2）。`ncs_sqlite_repository` 进一步覆盖管理控制面重启恢复、管理审计事务回滚、站点乐观锁、动态服务费报价、在线备份与隔离恢复验证、ML 任务超时转入 `TIMED_OUT`、备份保留轮换、SQL 层用户/流程过滤分页与站点/设备活动流程定向查询（v5 索引迁移）；设备独立“重启中”状态。WebSocket 通知仍依赖第 7 节。

## 6. P0：管理端 REST 接口（31项）

### 6.1 认证与用户管理（7项）

- [x] `POST /api/v1/admin/auth/login`
- [x] `POST /api/v1/admin/auth/reauth`
- [x] `POST /api/v1/admin/auth/logout`
- [x] `GET /api/v1/admin/users`
- [x] `GET /api/v1/admin/users/{userId}`
- [x] `PUT /api/v1/admin/users/{userId}/status`
- [x] `GET /api/v1/admin/users/{userId}/orders`

### 6.2 电站、设备与价格（13项）

- [x] `GET /api/v1/admin/stations`
- [x] `POST /api/v1/admin/stations`
- [x] `PUT /api/v1/admin/stations/{stationId}`
- [x] `POST /api/v1/admin/stations/{stationId}/disable`
- [x] `POST /api/v1/admin/stations/{stationId}/enable`
- [x] `GET /api/v1/admin/chargers`
- [x] `POST /api/v1/admin/chargers/batch`
- [x] `PUT /api/v1/admin/chargers/{chargerId}/status`
- [x] `POST /api/v1/admin/chargers/{chargerId}/restart-commands`
- [x] `GET /api/v1/admin/device-commands/{commandNo}`
- [x] `GET /api/v1/admin/tariffs`
- [x] `POST /api/v1/admin/tariffs`
- [x] `POST /api/v1/admin/price-adjustments`

### 6.3 流程、统计、审计与备份协议（8项）

- [x] `GET /api/v1/admin/flows`
- [x] `POST /api/v1/admin/flows/{flowNo}/force-releases`
- [x] `GET /api/v1/admin/stats/revenue`
- [x] `GET /api/v1/admin/stats/charger-status`
- [x] `GET /api/v1/admin/audit-logs`
- [x] `POST /api/v1/admin/backups` 的鉴权、DTO和任务编排层。
- [x] `GET /api/v1/admin/backups` 的鉴权和DTO层。
- [x] `POST /api/v1/admin/backups/{backupNo}/verifications` 的鉴权、DTO和任务编排层。

> 备份文件生成、SQLite Online Backup 和恢复验证的数据层已随 6.3 完成（见上方完成证据），不再排除。

### 6.4 预测接口（3项）

- [x] `GET /api/v1/admin/predictions`
- [x] `POST /api/v1/admin/ml-tasks`
- [x] `GET /api/v1/admin/ml-tasks/{taskNo}`

### 6.5 管理业务运行时

- [x] 实现用户冻结/解冻、会话撤销、活动充电保留和审计。
- [x] 实现电站启用/停用及活动流程拦截。
- [x] 实现电桩状态变更及活动设备保护。
- [x] 实现强制释放、设备后续状态、队列递补、用户通知和审计。
- [x] 实现远程重启命令状态机和2秒模拟执行。
- [x] 实现充电中设备受控结束、结算或待处理结算后重启。
- [x] 实现基础价格版本和 ML 服务费调整的应用规则。
- [x] 实现营收趋势、订单量、设备状态和健康度统计服务。
- [x] 实现只读审计查询及敏感字段过滤。

## 7. P0：WebSocket 实时服务

- [x] 实现 `wss://127.0.0.1:8443/api/v1/events`。
- [x] 通过握手 `Authorization` 鉴权，禁止 URL 携带 Token。
- [x] 实现唯一 `eventId` 和单调递增 `sequence`。
- [x] 实现用户、管理员和大屏的接收范围隔离。
- [x] 发布 `flow.updated`。
- [x] 发布 `charge.progress`，允许客户端丢弃旧进度。
- [x] 发布 `charger.statusChanged`。
- [x] 发布 `order.settled`。
- [x] 发布 `dashboard.refresh`。
- [x] 发布 `session.revoked` 并立即终止失效会话。
- [x] 实现断线重连和序号跳跃后的 REST 快照恢复约定。
- [x] 实现慢客户端、积压、心跳、关闭和服务重启处理。
- [x] 确保通知不含完整手机号、余额、令牌、验证码或完整订单。

WebSocket 模块完成证据（2026-09-03）：

- 需求：`NFR-P-05`（100 在线会话）、`NFR-S-05`（通知不含手机号/余额/完整订单）、`UC-W-04`（大屏会话与推送停止）、`NFR-C-04`，以及接口文档第 13 节（本节实现前已按维护规则先行扩充 13.1～13.5：握手拒绝码、eventId/sequence 语义、各事件 data 白名单、心跳与关闭码、积压与投递语义）。
- 核心：`core/application/event_hub.{h,cpp}` 实现全局单调 `sequence`、`EV+YYYYMMDD+8位日序号` 的 `eventId`、对等端注册表、接收范围路由（用户/管理员/大屏/精确会话）、60 秒滑动窗口积压策略（溢出先弃 progress、仍溢出 close 1013）、30 秒静默发应用层 `{"type":"ping"}`、60 秒无 pong close 4000、心跳两阶段存活复验（失效 close 4002）与 `session.revoked` 帧后 close 4001。积压采用滑动窗口而非实体队列：Crow 的写缓冲无完成回调，实体队列无法排空，窗口按单位时间限制 hub→传输层投递量（对计划的偏离，见接口文档 13.5）。
- 接入：`server/websocket/websocket_routes.{h,cpp}` 以 Crow 内置 `WebSocketRule` 注册升级路由，`authorizeHandshake` 复用 `SessionManager::parseBearer/authenticate`，拒绝 ML 令牌（403）；`outbox_dispatcher.{h,cpp}` 每 2 秒轮询 outbox 并富化发布（flow.updated 反查流程与队列位置、order.settled 只含六字段白名单、charger.statusChanged 反查设备），投递后标记，异常走 attempt 计数；`progress_pusher.{h,cpp}` 每 1 秒按连接用户推送充电进度（复用 `ChargeFlowService::progress`，字段与 §5.7 一致）。设备状态事件由 7 处充电流程转换（分配/递补/取消/过期/结算/受控结算/强制释放）与 3 处管理操作（状态变更/重启命令/重启完成）同事务写入 outbox（`aggregate_type='charger'`）。
- 会话撤销：`SessionManager::setRevocationObserver` 在五个撤销路径（revoke/revokeForPrincipal/revokePrincipal/revokeOtherSessions/issue 同设备替换）锁外通知；`main.cpp` 把通知投递到阻塞池（Crow 的 close 在 io 线程上会 dispatch 立即执行、越过已 post 的帧，阻塞池投递保证帧先于 4001 到达）。
- 缺陷修复：Crow 只有单个 tick 槽位，原 `app.tick(15s)` 被 `app.tick(1min)` 静默覆盖、15 秒维护任务从未运行；`server/runtime/periodic_scheduler.{h,cpp}` 以单一 1 秒 tick 驱动五档节奏（5 秒心跳检查/1 秒进度/2 秒投递/15 秒维护/60 秒清理），维护任务首次真正生效。
- 审查修复（2026-09-04，见接口文档 13.1～13.5 同步变更）：
  - P1 调度停摆：五档任务中仅 outbox 一档有 try/catch，任一瞬时 SQLite 异常会让 done() 不再被调用、对应档位永久停摆；现在 1/15/60 秒任务体全量兜底，`PeriodicScheduler` 对抛异常的作业自行复位（`inFlight` 改为 `std::atomic<bool>` 消除跨线程数据竞争）。
  - P1 同 token 重连：旧连接要等心跳 60～90 秒才被清掉，期间同 sessionId 重连被查重拒绝并误报 1013 "server at capacity"；现在注册时同会话新连接直接取代旧连接（旧连接 1001 关闭），注销与 pong 均按对等端身份校验，旧连接的 onclose/pong 不会误伤新连接。
  - P1 eventId 重复：4 位日序号取模使日事件量超 1 万必然重复，10 个充电用户约 17 分钟耗尽；改为 8 位日序号（每天 10⁸ 条），契约 13.2 同步更新，新增 10002 条单日唯一性测试。
  - P1 持锁 close 死锁窗口：Crow 的 close 经 asio::dispatch 在 io 线程内联执行，可能经 onclose 重入 hub 的非递归锁；`tickHeartbeat`（4000/4002）、`publish` 溢出（1013）、`notifySessionRevoked`（4001）全部改为先摘除注册表、放锁后关闭。
  - P2：握手超容量改在 onaccept 以 403 拒绝（13.1），onopen 的 1013 仅剩竞争性残余路径；心跳检查从 30 秒一档改 5 秒一档，ping/关闭误差从最坏 60/90 秒收敛到 5 秒内；超长帧补 Crow 补丁（`cmake/patches/crow-websocket-payload-1009.patch`）先发 1009 关闭帧再断传输；outbox 投递与标记之间以进程内已投递集合去重，标记失败只重试标记不重发；失败行改为指数退避（5 秒起、上限 300 秒）后再进死信，避免瞬时故障 20 秒误转死信；order.settled 订单缺失时不再发残缺帧，按死信排空；撤销通知的 submit 返回值不再丢弃，队列满时回退为无帧 4001 关闭。
- 验证（2026-09-03）：19 个 CTest 目标全部通过。新增 `ncs_websocket_hub`（范围隔离、序号单调、eventId 格式与 UTC 日翻转、积压策略、心跳/存活复验、撤销帧先于关闭、注册表规则、JSON 转义）、`ncs_websocket_dispatcher`（三类事件数据形状与富化、order.settled 无小票字段、范围路由、dashboard.refresh 节流、关停留行）、`ncs_websocket_routes`（四种令牌握手裁决，ML 拒绝唯一覆盖途径）、`ncs_periodic_scheduler`（到期/防重入/完成计时）；扩展 `ncs_sqlite_repository`（outbox 轮询/标记/attempt 转死信/charger 聚合行）、`ncs_server_config`（三项 WebSocket 配置）、`ncs_security_services`（观察者五路径触发且锁外回调）。烟雾测试以标准库自实现 WS 客户端走真实 wss：握手拒绝 401、`session.ready`、建流程收 `flow.updated`、充电收 `charge.progress`、结算收 `order.settled`（无小票字段）、管理员收结算与设备释放/状态变更事件、用户 B 收不到任何外来业务事件、注销收 `session.revoked` 后 close 4001、心跳 ping 与 pong 存活；`git diff --check` 通过。

## 8. P1：Dashboard 与 ML 服务连接

### 8.1 Dashboard（3个已定义接口）

- [x] `POST /api/v1/dashboard/auth/login`
- [x] `POST /api/v1/dashboard/auth/logout`
- [x] `GET /api/v1/dashboard/summary`
- [x] 实现运营管理员与决策查看者的服务端角色校验。
- [x] 每30秒生成完整快照并原子替换 `dashboard.json`。
- [x] 生成失败时保留最近成功快照并标记陈旧状态。
- [x] 数据变化后发布 `dashboard.refresh`。

### 8.2 ML 内部接口（4项）

- [x] `GET /api/v1/internal/ml/features/hourly`
- [x] `POST /api/v1/internal/ml/model-versions`
- [x] `POST /api/v1/internal/ml/predictions/batch`
- [x] `POST /api/v1/internal/ml/tasks/{taskNo}/completion`
- [x] 实现限定任务、作用域和有效期的一次性 ML Token。
- [x] 实现 Crow 启动、监控和停止 Python ML 子进程。
- [x] 限制同时最多一个训练任务和一个预测任务。
- [x] 实现训练10分钟、预测2分钟超时。
- [x] 重复任务请求返回当前任务，不重复启动。
- [x] 任务失败时保留最近成功结果并标记过期。

实现记录（2026-09-04）：`DashboardService` 生成零数据安全的完整快照，`DashboardRoutes` 使用独立 Dashboard Token 并通过同目录临时文件原子替换离线快照；写入失败只把内存快照标记为陈旧，不覆盖最近成功文件。SQLite v6 新增连续小时聚合、模型版本、负荷预测与持久化数据版本，ML 路由同时校验回环来源、任务令牌、任务号和任务类型作用域。`MlProcessManager` 通过标准输入传递令牌，监控异常退出并支持超时终止；训练产物先写任务专属暂存文件，校验 SHA-256 且任务成功后才激活，失败或不合格模型不覆盖最近成功模型。Python 管线固定随机种子并实现 168 小时滞后、滚动均值、版本化节假日日历、模拟天气、随机森林与朴素基线评估。

验证（2026-09-04）：新增 `ncs_dashboard_ml_routes` 覆盖 Dashboard 鉴权/完整空快照/原子落盘、ML 回环限制、特征游标、模型合格判定、令牌完成即撤销及预测幂等回写；新增 `ncs_ml_process_manager` 覆盖子进程异常退出回写。全量 CTest 21/21 通过；真实 HTTPS 烟雾测试已覆盖 Dashboard 登录、快照、WebSocket 刷新和重复退出，并由管理接口启动真实 Python `PREDICT` 子进程，以基线模型完成特征分页、批量回写和任务完成。Python 独立断言覆盖 168 小时样本门槛、无模型回退及繁忙/空闲桩公式。当前机器未安装 `numpy/joblib/scikit-learn`，所以随机森林训练和固定测试集优于基线的验收仍需安装 `ml/requirements.txt` 后执行；第 10 节完整 ML 训练集成项保持未勾选。

复审整改证据（2026-09-04，第二轮审查 15 项核实后修复）：

- F1 幂等与结算恢复：`core/application/idempotency_service.cpp` 只缓存 2xx 成功结果，4xx 客户端/业务错误（如 VersionConflict 409）与 5xx 同样释放密钥，同键重试可重新执行；`core/application/charge_flow_service.cpp` 结算 catch 持久化状态 80 时不再递增版本，同键同体重试直接通过 validFlowVersion 恢复。测试：`ncs_idempotency_service` 新增 4xx 释放与重试断言；`ncs_sqlite_repository` 结算失败注入后用失败前版本重试成功。
- F7 outbox 畸形行：`server/websocket/outbox_dispatcher.cpp` 用 `std::from_chars` 防御性解析 aggregateId，畸形/非正数直接死信排干，不再反复消耗重试预算。
- F13 模型产物：`infrastructure/files/model_artifact_store.*` 保留期对齐 90 天预测清理，`cleanupExpired(keepArtifactPath)` 永久保留最新合格模型的产物；`server/main.cpp` 维护任务传入 `latestQualifiedModel` 路径。`server/runtime/ml_process_manager.cpp` 在合格模型产物缺失/校验失败时显式把任务置为 FAILED（errorSummary="合格模型产物缺失或校验失败"），不再静默退化为 BASELINE 顶替有效预测。测试：`ncs_dashboard_ml_routes` 新增清理保留与过期删除断言。
- F3 特征重建：`infrastructure/sqlite/sqlite_repository.cpp` `refreshHourlyMetrics` 改为按天分块 UPSERT（ON CONFLICT DO UPDATE），每次写事务只覆盖 24 个桶，读者不再看到删除间隙；迁移 v7 新增 `ix_order_status_settled(status,settled_at)` 与 `ix_order_status_started(status,COALESCE(started_at,created_at))` 索引，`kLatestSchemaVersion=7`。
- F4 完成路径：`core/application/analytics_service.cpp` `MlService::complete` 把产物 finalize 文件 I/O 移出 SQLite 写事务，事务内重新校验任务与版本，避免文件复制延长写锁窗口。
- F10/F14 配置：`server/runtime/server_config.*` 未显式指定时 `chargeTimeScale` 按环境派生（开发 60、其余 1）；ServerConfig 记录资产目录探测结果，`server/main.cpp` 在回退到可执行文件目录时输出醒目 WARN 日志。测试：`ncs_server_config` 新增生产默认 1 与显式覆盖断言。
- 验证（2026-09-04）：全量构建通过；CTest 21/21 全部通过，`ncs_server_smoke` 以真实 HTTPS 全链路通过；`git diff --check` 通过。

## 9. 各前端的后端联调支持

### 9.1 Qt 用户端

- [ ] 提供共享 Qt HTTP 客户端封装及 DTO。
- [ ] 支持 Token、请求 ID、幂等键和超时重试；写请求重试复用原幂等键。
- [ ] 提供 ErrorCode 到用户提示/页面动作的映射。
- [ ] 提供 WebSocket 客户端、重连、序号检测和 REST 补快照。
- [ ] 联调登录、资料、钱包、电站、流程、计费、结算和订单完整链路。
- [ ] 验证加载、空数据、失败、会话失效及恢复状态。

### 9.2 Qt 管理端

- [ ] 提供独立管理员 HTTP/WebSocket 客户端及 DTO。
- [ ] 支持重新验证、幂等键、资源版本和冲突后刷新。
- [ ] 联调用户、电站、电桩、流程、价格、统计、审计和预测。
- [ ] 联调远程重启、强制释放及异步命令状态。
- [ ] 验证权限不足、会话撤销、空图表、服务断开和恢复状态。

### 9.3 Vue/ECharts 大屏

- [ ] 提供受权登录和角色信息契约。
- [ ] 联调 Dashboard 完整快照。
- [ ] 联调 WebSocket 刷新通知。
- [ ] 验证断线后30秒轮询、版本跳跃补快照和陈旧数据提示。
- [ ] 验证决策查看者只能读取 Dashboard 数据。

### 9.4 Python ML 子进程

- [ ] 提供任务参数、任务令牌和错误摘要契约。
- [ ] 联调特征游标分页、模型登记、批量预测回写和任务完成。
- [ ] 验证超时、重复启动、子进程异常退出和最近成功结果回退。

## 10. 非数据库验证清单

- [ ] 为全部已冻结 REST 路由建立契约测试。
- [ ] 每个写接口覆盖正常、权限、字段边界、非法状态、幂等冲突和版本冲突。
- [ ] 每个查询接口覆盖空数据、分页上限、过滤、排序白名单和权限。
- [ ] 覆盖 WebSocket 鉴权、接收范围、事件顺序、断线和补快照。
- [ ] 覆盖验证码冷却、过期、错误次数和日志过滤。
- [ ] 覆盖用户与管理员会话数量、过期、退出和撤销。
- [ ] 覆盖排队、报价、预约、充电、结算的状态机单元测试。
- [ ] 覆盖用户取消、管理员强制释放和远程重启的幂等测试。
- [ ] 覆盖计费公式、时间倍率和进程恢复测试。
- [ ] 覆盖头像伪造格式、超限尺寸、元数据剥离和路径安全。
- [ ] 覆盖越权、未知字段、无效排序、请求超限、限流和日志脱敏。
- [ ] 使用 Python 虚拟客户端验证100个在线会话和 WebSocket 连接。
- [ ] 验证 HTTPS API 持续20请求/秒及50请求/秒短时峰值。
- [ ] 执行用户端—服务端关键路径端到端测试。
- [ ] 执行管理端—服务端关键路径端到端测试。
- [ ] 执行 Dashboard、WebSocket 和 ML 子进程集成测试。
- [ ] 执行 Ubuntu 构建、服务启动、健康检查及优雅关闭烟雾测试。

## 11. 临时里程碑与退出条件

### M0：契约冻结

- 第2节决策已同步到 SRS、接口文档和数据库设计。
- SRS、接口文档、共享 DTO 和错误码一致。
- REST 路由清单和 WebSocket 事件清单冻结。

### M1：服务端骨架可运行

- `ncs_server` 可构建、启动和优雅退出。
- HTTPS、统一响应、日志、鉴权入口和健康检查可验证。

### M2：用户端纵向闭环

- 短信登录 → 电站查询 → 排队/报价 → 预约 → 充电 → 结算 → 订单查询通过。
- Qt 用户端断线重连和流程恢复通过。

### M3：管理端纵向闭环

- 管理员登录 → 用户/站点/设备管理 → 强制释放/重启 → 统计刷新通过。
- 权限、重新验证、审计和版本冲突处理通过。

### M4：大屏与 ML

- Dashboard 受权快照、实时刷新和离线降级通过。
- ML 任务启动、内部接口、超时和失败回退通过。

### M5：非数据库后端验收

- 已冻结 REST 和 WebSocket 契约测试全部通过。
- 关键端到端流程、并发容量、安全及恢复测试通过。
- 需求追踪关系、运行说明和验证记录已更新。

## 12. 维护规则

- 本文件只记录执行状态，不在这里定义新的业务规则。
- 文档冲突先修改 SRS，再同步接口文档、实现和测试。
- 完成任务时必须附代码位置、需求编号和实际测试证据。
- 数据库任务另行跟踪，不得因为本文件排除数据库而弱化事务和一致性要求。
- 正式任务系统建立或需求追踪矩阵更新后，应归档或删除本临时文件，避免形成第二需求基线。
