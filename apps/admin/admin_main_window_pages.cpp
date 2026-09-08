#include "admin_charts.h"
#include "admin_main_window.h"
#include "admin_main_window_utils.h"
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QTableWidget>
#include <QVBoxLayout>
namespace ncs::admin
{
namespace
{
QPushButton* action(const QString& text, bool secondary = false)
{
    auto* b = new QPushButton(text);
    if (secondary)
        b->setObjectName("secondaryButton");
    return b;
}
} // namespace
QWidget* AdminMainWindow::metric(const QString& title, const QString& detail, const QString& name,
                                 QLabel** out)
{
    auto* card = new QFrame;
    card->setObjectName("card");
    auto* l = new QVBoxLayout(card);
    l->setContentsMargins(18, 16, 18, 16);
    l->setSpacing(8);
    auto* t = new QLabel(title);
    t->setObjectName("metricTitle");
    *out = new QLabel(QStringLiteral("—"));
    (*out)->setObjectName(name);
    (*out)->setStyleSheet("font-size:27px;font-weight:800;color:#123347;");
    auto* d = new QLabel(detail);
    d->setObjectName("muted");
    d->setWordWrap(true);
    l->addWidget(t);
    l->addWidget(*out);
    l->addWidget(d);
    return card;
}
QWidget* AdminMainWindow::createDashboardPage()
{
    QVBoxLayout* l;
    auto* page = newPage(0, QStringLiteral("运营总览"),
                         QStringLiteral("查看营收与设备状态，及时掌握运营变化 · 北京时间"), &l);
    auto* metrics = new QGridLayout;
    metrics->setSpacing(12);
    metrics->addWidget(metric(QStringLiteral("今日营收"), QStringLiteral("今日已结算订单"),
                              "todayRevenue", &todayRevenue_),
                       0, 0);
    metrics->addWidget(metric(QStringLiteral("本月营收"), QStringLiteral("本月 1 日至今"),
                              "monthRevenue", &monthRevenue_),
                       0, 1);
    metrics->addWidget(metric(QStringLiteral("可运营电桩"), QStringLiteral("平台设备运营统计"),
                              "operationalChargers", &operationalChargers_),
                       0, 2);
    metrics->addWidget(metric(QStringLiteral("注册用户"), QStringLiteral("平台用户总数"),
                              "registeredUsers", &registeredUsers_),
                       0, 3);
    for (int i = 0; i < 4; ++i)
        metrics->setColumnStretch(i, 1);
    l->addLayout(metrics);
    auto* panels = new QHBoxLayout;
    panels->setSpacing(12);
    auto* revenueCard = new QFrame;
    revenueCard->setObjectName("card");
    auto* rl = new QVBoxLayout(revenueCard);
    rl->setContentsMargins(18, 16, 18, 12);
    auto* bar = new QHBoxLayout;
    bar->addWidget(new QLabel(QStringLiteral("营收趋势 / 元")), 1);
    revenueRange_ = new QComboBox;
    revenueRange_->setObjectName("revenueRange");
    revenueRange_->addItem(QStringLiteral("近 7 日"), 7);
    revenueRange_->addItem(QStringLiteral("近 30 日"), 30);
    bar->addWidget(revenueRange_);
    rl->addLayout(bar);
    trend_ = new AdminTrendWidget;
    rl->addWidget(trend_, 1);
    auto* statusCard = new QFrame;
    statusCard->setObjectName("card");
    auto* sl = new QVBoxLayout(statusCard);
    sl->setContentsMargins(18, 16, 18, 12);
    sl->addWidget(new QLabel(QStringLiteral("电桩状态")));
    statusChart_ = new AdminStatusChart;
    statusChart_->setObjectName("chargerStatusChart");
    sl->addWidget(statusChart_, 1);
    health_ = new QLabel(QStringLiteral("健康度 —"));
    health_->setObjectName("chargerHealth");
    sl->addWidget(health_);
    panels->addWidget(revenueCard, 3);
    panels->addWidget(statusCard, 2);
    l->addLayout(panels);
    auto* detail = new QHBoxLayout;
    detail->addWidget(new QLabel(QStringLiteral("每日营收与订单")), 1);
    auto* note = new QLabel(QStringLiteral("仅统计已完成并结算成功的订单"));
    note->setObjectName("muted");
    detail->addWidget(note);
    l->addLayout(detail);
    revenueTable_ =
        makeTable({QStringLiteral("日期"), QStringLiteral("营收"), QStringLiteral("订单量")});
    revenueTable_->setObjectName("revenueTable");
    revenueTable_->setMinimumHeight(180);
    l->addWidget(revenueTable_, 1);
    connect(revenueRange_, &QComboBox::currentIndexChanged, this,
            [this]
            {
                if (api_.hasSession())
                    refreshOverview();
            });
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setWidget(page);
    return scroll;
}
QWidget* AdminMainWindow::createStationsPage()
{
    QVBoxLayout* l;
    auto* page = newPage(1, QStringLiteral("充电站管理"),
                         QStringLiteral("维护站点信息与可用状态，选择站点可查看所属设备"), &l);
    auto* row = new QHBoxLayout;
    stationSearch_ = new QLineEdit;
    stationSearch_->setObjectName("stationSearch");
    stationSearch_->setPlaceholderText(QStringLiteral("搜索站点名称或地址"));
    auto* search = action(QStringLiteral("搜索"), true);
    auto* add = action(QStringLiteral("新增站点"));
    auto* toggle = action(QStringLiteral("启用 / 停用"), true);
    toggle->setObjectName("toggleStation");
    auto* devices = action(QStringLiteral("查看设备"), true);
    row->addWidget(stationSearch_, 1);
    row->addWidget(search);
    row->addSpacing(8);
    row->addWidget(devices);
    row->addWidget(toggle);
    row->addWidget(add);
    l->addLayout(row);
    stationTable_ = makeTable({QStringLiteral("站点编码"), QStringLiteral("站点名称"),
                               QStringLiteral("行政区编码"), QStringLiteral("运营状态")});
    stationTable_->setObjectName("stationTable");
    l->addWidget(stationTable_, 1);
    addPager(l, 1);
    auto reload = [this]
    {
        pageNumbers_[1] = 1;
        refreshStations();
    };
    connect(search, &QPushButton::clicked, this, reload);
    connect(stationSearch_, &QLineEdit::returnPressed, this, reload);
    connect(add, &QPushButton::clicked, this, &AdminMainWindow::addStation);
    connect(toggle, &QPushButton::clicked, this, &AdminMainWindow::removeStation);
    connect(devices, &QPushButton::clicked, this,
            [this]
            {
                const auto id = selectedId(stationTable_);
                if (id <= 0)
                {
                    notify(QStringLiteral("请先选择站点"), true);
                    return;
                }
                const int index = chargerStation_->findData(id);
                if (index < 0)
                {
                    notify(QStringLiteral("站点目录尚未加载，请刷新后重试"), true);
                    return;
                }
                chargerStation_->setCurrentIndex(index);
                navigation_->setCurrentRow(2);
            });
    mutationButtons_ << add << toggle;
    return page;
}
QWidget* AdminMainWindow::createChargersPage()
{
    QVBoxLayout* l;
    auto* page = newPage(2, QStringLiteral("充电桩管理"),
                         QStringLiteral("筛选设备、查看运行状态，按需执行维护操作"), &l);
    auto* filters = new QHBoxLayout;
    chargerSearch_ = new QLineEdit;
    chargerSearch_->setObjectName("chargerSearch");
    chargerSearch_->setPlaceholderText(QStringLiteral("搜索电桩编号"));
    chargerStation_ = new QComboBox;
    chargerStation_->setObjectName("chargerStation");
    chargerStation_->addItem(QStringLiteral("全部站点"), qint64(0));
    chargerStation_->setMaximumWidth(250);
    chargerStatus_ = new QComboBox;
    chargerStatus_->setObjectName("chargerStatus");
    chargerStatus_->addItem(QStringLiteral("全部状态"), -1);
    for (int i = 0; i < 5; ++i)
        chargerStatus_->addItem(chargerStatusText(i), i);
    auto* search = action(QStringLiteral("筛选"), true);
    filters->addWidget(chargerSearch_, 1);
    filters->addWidget(chargerStation_);
    filters->addWidget(chargerStatus_);
    filters->addWidget(search);
    l->addLayout(filters);
    auto* actions = new QHBoxLayout;
    auto* hint = new QLabel(QStringLiteral("选中设备后操作；使用中的订单由服务端保护"));
    hint->setObjectName("muted");
    hint->setWordWrap(true);
    auto* status = action(QStringLiteral("修改状态"), true);
    status->setObjectName("changeChargerStatus");
    auto* restart = action(QStringLiteral("远程重启"));
    restart->setObjectName("restartCharger");
    actions->addWidget(hint, 1);
    actions->addWidget(status);
    actions->addWidget(restart);
    l->addLayout(actions);
    chargerTable_ =
        makeTable({QStringLiteral("电桩编号"), QStringLiteral("所属站点"), QStringLiteral("类型"),
                   QStringLiteral("功率"), QStringLiteral("状态"), QStringLiteral("充电次数"),
                   QStringLiteral("累计时长")});
    chargerTable_->setObjectName("chargerTable");
    l->addWidget(chargerTable_, 1);
    addPager(l, 2);
    auto reload = [this]
    {
        pageNumbers_[2] = 1;
        refreshChargers();
    };
    connect(search, &QPushButton::clicked, this, reload);
    connect(chargerSearch_, &QLineEdit::returnPressed, this, reload);
    connect(status, &QPushButton::clicked, this, &AdminMainWindow::updateChargerStatus);
    connect(restart, &QPushButton::clicked, this, &AdminMainWindow::restartCharger);
    mutationButtons_ << status << restart;
    return page;
}
QWidget* AdminMainWindow::createUsersPage()
{
    QVBoxLayout* l;
    auto* page = newPage(3, QStringLiteral("用户管理"),
                         QStringLiteral("查询用户状态与余额，冻结操作保留已有订单"), &l);
    auto* row = new QHBoxLayout;
    userSearch_ = new QLineEdit;
    userSearch_->setObjectName("userSearch");
    userSearch_->setMaxLength(11);
    userSearch_->setPlaceholderText(QStringLiteral("完整手机号或后四位"));
    auto* search = action(QStringLiteral("查询"), true);
    auto* toggle = action(QStringLiteral("冻结 / 解冻"));
    toggle->setObjectName("toggleUserStatus");
    row->addWidget(userSearch_, 1);
    row->addWidget(search);
    row->addSpacing(12);
    row->addWidget(toggle);
    l->addLayout(row);
    userTable_ =
        makeTable({QStringLiteral("用户 ID"), QStringLiteral("手机号"), QStringLiteral("昵称"),
                   QStringLiteral("钱包余额"), QStringLiteral("注册时间"), QStringLiteral("状态")});
    userTable_->setObjectName("userTable");
    l->addWidget(userTable_, 1);
    addPager(l, 3);
    auto reload = [this]
    {
        pageNumbers_[3] = 1;
        refreshUsers();
    };
    connect(search, &QPushButton::clicked, this, reload);
    connect(userSearch_, &QLineEdit::returnPressed, this, reload);
    connect(toggle, &QPushButton::clicked, this, &AdminMainWindow::toggleUserStatus);
    mutationButtons_ << toggle;
    return page;
}
QWidget* AdminMainWindow::createPredictionsPage()
{
    QVBoxLayout* l;
    auto* page =
        newPage(4, QStringLiteral("智能预测"),
                QStringLiteral("查看未来负荷与空闲设备预估，高峰时段优先关注 · 北京时间"), &l);
    auto* row = new QHBoxLayout;
    predictionStation_ = new QComboBox;
    predictionStation_->setObjectName("predictionStation");
    predictionStation_->addItem(QStringLiteral("全部站点"), qint64(0));
    predictionHorizon_ = new QComboBox;
    predictionHorizon_->setObjectName("predictionHorizon");
    for (int h : {1, 6, 24})
        predictionHorizon_->addItem(QStringLiteral("未来 %1 小时").arg(h), h);
    predictionHorizon_->setCurrentIndex(2);
    auto* refresh = action(QStringLiteral("查询结果"), true);
    auto* run = action(QStringLiteral("运行预测"));
    run->setObjectName("runPrediction");
    row->addWidget(predictionStation_, 1);
    row->addWidget(predictionHorizon_);
    row->addWidget(refresh);
    row->addWidget(run);
    l->addLayout(row);
    predictionTable_ = makeTable({QStringLiteral("目标时段"), QStringLiteral("站点"),
                                  QStringLiteral("预测电量"), QStringLiteral("预计空闲桩"),
                                  QStringLiteral("负荷提示"), QStringLiteral("数据状态")});
    predictionTable_->setObjectName("predictionTable");
    l->addWidget(predictionTable_, 1);
    connect(refresh, &QPushButton::clicked, this, &AdminMainWindow::refreshPredictions);
    connect(run, &QPushButton::clicked, this, &AdminMainWindow::runPrediction);
    mutationButtons_ << run;
    return page;
}
} // namespace ncs::admin
