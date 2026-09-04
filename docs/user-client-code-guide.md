# PE1 用户端代码阅读指南（从零开始）

这份文档对应 `apps/user/`。阅读目标不是背每一行，而是能回答：**程序从哪里开始？一个点击如何变成页面变化？数据放在哪里？以后后端如何接入？**

## 0. 先记住一张图

```text
main.cpp
  ├─ 创建 QApplication（Qt 程序运行环境）
  ├─ 读取配置、启动日志、应用主题
  ├─ 创建 MockUserClientService（当前演示数据/业务状态）
  └─ 创建 UserMainWindow（所有页面）
       ├─ 登录、首页、选桩、充电、订单、我的等页面
       ├─ ui/ 中的可复用控件
       └─ 调用 service_ 读取或修改数据

将来：UserClientService → RestUserClientService → UserApi → ApiClient → 后端 REST
当前：UserClientService → MockUserClientService（不需要后端也能演示）
```

程序不是每个页面一个可执行文件；它只有一个 `ncs_user` 窗口，内部用 `QStackedWidget` 切换“当前页”。

## 1. 文件夹和文件的作用

| 位置 | 你把它理解成什么 | 主要职责 |
| --- | --- | --- |
| `main.cpp` | 程序总开关 | 创建 Qt 应用、主题、配置、日志、Mock Service 和主窗口 |
| `user_main_window.h` | 主窗口的“目录/说明书” | 声明有哪些页面、控件指针和状态变量 |
| `user_main_window.cpp` | 主窗口主体 | 创建页面容器、首页、选桩、充电、小票及按钮连接 |
| `user_main_window_login.cpp` | 登录页 | 手机号校验、验证码倒计时、登录按钮 |
| `user_main_window_profile.cpp` | 我的页 | 头像、昵称、余额、充值、退出 |
| `user_main_window_orders.cpp` | 订单页 | 表格、空状态、订单小票弹窗 |
| `user_main_window_navigation.cpp` | 导航页 | 驾车/步行、浏览器路线链接 |
| `user_main_window_state.cpp` | 页面状态控制器 | 跳页、刷新充电数据、Toast、Esc 返回 |
| `user_demo_service.h/.cpp` | 当前的“假后端” | 定义模型和接口，保存 Mock 的订单/余额/充电状态 |
| `ui/` | 小组件箱 | 主题、Dock、站点卡、电桩卡、SoC 仪表、站点列表 |
| `net/api_client.*` | 网络总管 | 异步 HTTP、Token、超时、统一响应错误 |
| `net/user_api.*` | 用户端接口字典 | 唯一存放 `/api/v1/user/*` 路径和请求字段的位置 |

`CMakeLists.txt` 不是业务代码。它只是告诉 CMake：哪些 `.cpp` 文件要编译为 `ncs_user`，并链接 `Qt6::Widgets`、`Qt6::Network` 等库。

## 2. 从启动开始读：`main.cpp`

核心逻辑可概括为：

```cpp
QApplication app(argc, argv);              // 让 Qt 接管窗口和事件循环
AppTheme::apply(app);                      // 应用统一 QSS 样式
MockUserClientService service;             // 创建当前的离线数据源
UserMainWindow window(service);            // 把数据源交给界面
window.show();                             // 显示窗口
return app.exec();                         // 开始等待鼠标、键盘、定时器等事件
```

`app.exec()` 是事件循环：它启动后程序不会从上到下重复执行，而是在用户点击按钮、输入文字、定时器到点时调用相应的回调函数。

额外的 `--smoke-test` 参数用于自动退出，便于 CI/离屏验证；`--api-request-code <手机号>` 用于不打开业务窗口就测试验证码 REST 路由。

## 3. 主窗口为何要保存一堆 `QLabel*`、`QPushButton*`

在 `user_main_window.h` 中，例如：

```cpp
QLabel* chargeAmount_ = nullptr;
QPushButton* settleButton_ = nullptr;
UserClientService& service_;
```

- `QLabel*`：指向屏幕上的一段文字控件。保存它，之后 `refreshCharge()` 才能改金额文本。
- `QPushButton*`：保存按钮，之后可 `setEnabled(false)` 防止重复结算。
- `= nullptr`：刚创建窗口时还没有对象，先置空，避免野指针。
- `UserClientService&`：引用，不复制 Service；主窗口始终操作 `main.cpp` 创建的同一个数据源。

Qt 控件大多有父对象：例如 `new QLabel(page)`。`page` 被销毁时会自动销毁它的子控件，因此一般不需要手动 `delete` 每个 Label。

## 4. 页面切换：`QStackedWidget`

构造函数依次把页面放入 `pages_`：

```cpp
pages_->addWidget(createLoginPage());   // 索引 0
pages_->addWidget(createHomePage());    // 索引 1
pages_->addWidget(createDetailPage());  // 索引 2
```

显示某页不是新开窗口，而是：

```cpp
pages_->setCurrentIndex(kHomePage);
```

所以 `showHome()`、`showDetail()`、`showCharge()` 的主要工作有两类：

1. 准备数据（例如根据站点 ID 填标题和电桩卡）；
2. 切换当前索引、显示或隐藏底部 Dock。

`keyPressEvent()` 重写了 `QMainWindow` 的按键处理：按 `Esc` 时根据当前页面回到上一层；若正在充电，就拦截返回并提示用户先结算。这是客户端页面逻辑，不依赖后端。

## 5. 最重要的 Qt 语法：信号与槽

下面代码来自站点列表：

```cpp
connect(refreshButton, &QPushButton::clicked,
        this, &StationListWidget::refresh);
```

把它读成一句话：**当 `refreshButton` 被点击时，调用当前对象的 `refresh()` 函数。**

- `clicked` 是 Qt 已定义的“信号”；
- `refresh` 是“槽”，这里就是普通成员函数；
- `connect` 不会立即执行 `refresh()`，而是登记一个事件规则。

再看自定义组件：

```cpp
connect(card, &StationCard::selected,
        this, &StationListWidget::stationSelected);
```

意思是：站点卡只负责发出“我被选中了，ID 是多少”的信号；站点列表把信号继续往外传。卡片不直接跳转页面，这样组件就能复用，也不会互相依赖。

## 6. Lambda：把一小段点击后逻辑写在按钮旁边

```cpp
connect(login, &QPushButton::clicked, this, [this] {
    if (phoneEdit_->text().size() != 11) {
        notify(QStringLiteral("请输入正确的 11 位手机号"), true);
        return;
    }
    // 调用 service_.login，再决定是否进入首页
});
```

`[this] { ... }` 就是 Lambda（匿名函数）。

- 方括号的 `[this]` 叫“捕获”：允许这段小函数使用当前窗口的 `phoneEdit_`、`service_`、`notify()`；
- `return` 只退出这段 Lambda，不会退出整个程序；
- 适合很短的按钮业务逻辑；复杂逻辑应拆成普通成员函数，避免页面文件太长。

## 7. `Service` 接口和 Mock 状态机

`UserClientService` 是抽象接口。例如：

```cpp
virtual bool reserve(int stationId, const QString& chargerCode,
                     QString* userMessage) = 0;
```

- `virtual`：子类必须提供自己的实现；
- `= 0`：这是纯虚函数，接口自身不能直接创建对象；
- `MockUserClientService final : public UserClientService`：Mock 是接口的一个实现；`override` 让编译器检查函数签名没有写错；`final` 表示不再从 Mock 继续继承。

Mock 内部真正保存的状态是：

```text
balanceCent_                 当前余额（单位：分）
selectedStationId_           选中的站点
selectedChargerCode_         选中的桩
reserved_ / charging_        是否预约、是否充电
reservationRemainingSeconds_ 预约剩余秒数
elapsedSeconds_              演示运行秒数
activeOrderNo_ / orders_     当前订单和历史订单
```

一次 Mock 充电的状态变化：

```text
reserve() → reserved_=true，创建“已预约”订单
start()   → charging_=true，订单改为“充电中”
tick()    → 每秒增加 elapsedSeconds_
progress()→ 根据 elapsedSeconds_ 计算展示数据
settle()  → 扣余额、订单改“已完成”、清除活动订单
```

因此你看到电量和费用增长，并不是数据库实时写入；它是 `progress()` 按演示公式即时算出的数据。现有代码把每个真实秒按 60 秒充电时间演示，且 SoC 最高限制为 95%，这两个都是演示参数，不是正式计费规则。

## 8. 站点筛选与动态创建卡片

`StationListWidget::refresh()` 做了四件事：

1. 清掉上一次筛选生成的卡片；
2. 读取地区下拉框和搜索框文本；
3. 遍历 `QVector<StationSummary>`，用 `contains()` 判断是否匹配；
4. 为每个匹配站创建 `new StationCard(station)` 并插入竖向布局。

`QVector` 可理解为 C++ 的动态数组。`const StationSummary& station` 中：

- `const`：这里只读，不能误改原数据；
- `&`：引用，不复制整份站点对象，效率更好。

列表筛选目前在 Mock 数据上运行；接 REST 后，应由接口返回列表，再用相同卡片渲染，不把 JSON 解析写进组件。

## 9. 充电页与自绘 SoC 仪表

`refreshCharge()` 从 Service 得到 `ChargeProgress`，再分别写入时长、电量、金额、功率和 SoC 控件。

`ChargeSocGauge` 继承 `QWidget` 并重写 `paintEvent()`：

```cpp
void ChargeSocGauge::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.drawArc(...);  // 先画灰色轨道，再画绿色进度弧
}
```

`setValue()` 最后调用 `update()`，Qt 会在下一次绘制时回调 `paintEvent()`。这就是“自绘组件”的基本模式：**更新数据 → 请求重绘 → Qt 绘制**。

## 10. 网络代码该怎样理解

`ApiClient` 是通用 HTTP 工具，不知道“订单”是什么。它负责：

```text
拼基础 URL → 设置 Accept/Authorization/X-Request-ID → 发送异步请求
→ 收到 QNetworkReply → 解析 success/code/userMessage/data → 调用回调
```

`UserApi` 知道团队接口名称，例如 `POST /api/v1/user/flows`、`GET /flows/{flowNo}/progress`，但不认识任何 Qt 页面。这样后端路径变更只改 `net/user_api.cpp`，而不是到处搜索替换。

**重要现状：** 当前 UI 注入的是 `MockUserClientService`，所以点击页面不会真的发网络请求。`UserApi` 已按团队 V1 文档实现，下一步要做异步 `RestUserClientService`/ViewModel，把网络回调转换为页面刷新；绝不能用等待网络返回的同步循环，否则界面会卡死。

## 11. 其他常见语法速查

| 写法 | 含义 |
| --- | --- |
| `namespace ncs::user` | 把类放进命名空间，避免与别人的同名类冲突 |
| `#pragma once` | 头文件只被编译器包含一次 |
| `QStringLiteral("文字")` | 高效、安全地创建固定 Qt 字符串 |
| `QString("%1").arg(value)` | 用 `value` 替换 `%1`，用于显示文本 |
| `Q_UNUSED(x)` | 明确声明参数暂时不用，避免编译警告 |
| `qMin/qMax/qBound` | Qt 的最小、最大、范围限制函数 |
| `nullptr` | 空指针；比旧写法 `NULL` 更安全 |
| `auto* card = new StationCard(...)` | 让编译器推导类型；`*` 表示变量保存对象地址 |
| `const` | 不修改；用于保护数据和表达意图 |
| `QTimer::singleShot(...)` | 延迟执行一次，例如 Toast 3.2 秒后自动隐藏 |

## 12. 建议的阅读顺序和明天最低学习目标

按这个顺序打开代码，每个文件只看职责和关键函数：

1. `main.cpp`：知道程序如何启动、为何当前是 Mock。
2. `user_demo_service.h`：知道页面能向数据源要什么。
3. `user_demo_service.cpp`：重点看 `reserve/start/tick/progress/settle` 状态变化。
4. `user_main_window.h`：知道窗口存了哪些页面控件和状态。
5. `user_main_window_state.cpp`：知道跳页、刷新、Toast 和 Esc 逻辑。
6. `ui/station_list_widget.cpp`：理解信号槽、搜索和动态卡片。
7. `ui/charge_soc_gauge.cpp`：理解自绘组件。
8. `net/api_client.cpp`、`net/user_api.cpp`：只需理解“异步请求统一封装，尚未注入页面”。

明天最低限度要能自己说清：**我做的是用户端界面和演示状态机；Mock 是为了不依赖后端；正式模式通过 REST 异步访问后端；客户端绝不直接操作数据库；目前 REST 基础已就绪但页面真实联调是下一步。**
