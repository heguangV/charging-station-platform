#include "admin_main_window.h"

#include "admin_api_client.h"
#include "login_widget.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace ncs::admin
{

namespace
{
QTableWidget* makeTable(const QStringList& headers)
{
    auto* table = new QTableWidget;
    table->setColumnCount(headers.size());
    table->setHorizontalHeaderLabels(headers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setAlternatingRowColors(true);
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->setVisible(false);
    return table;
}

QLabel* heading(const QString& text)
{
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("pageTitle"));
    return label;
}
} // namespace

AdminMainWindow::AdminMainWindow(QWidget* parent)
    : QMainWindow(parent), loginPage_(nullptr), pages_(nullptr), workspacePages_(nullptr),
      navigation_(nullptr),
      stationTable_(nullptr), chargerTable_(nullptr), userTable_(nullptr),
      stationSearch_(nullptr), chargerSearch_(nullptr), userSearch_(nullptr),
      chargerStatus_(nullptr), statusLabel_(nullptr), apiClient_(new AdminApiClient(this))
{
    setWindowFlags(Qt::Window | Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint |
                   Qt::WindowCloseButtonHint);
    setWindowTitle(QStringLiteral("NCS 充电桩运营管理端"));
    setMinimumSize(960, 600);
    resize(1440, 900);
    styleApplication();
    seedDemoData();
    buildLoginPage();
    buildWorkspace();
    setCentralWidget(pages_);
    pages_->setCurrentIndex(0);
}

void AdminMainWindow::styleApplication()
{
    setStyleSheet(QStringLiteral(R"(
        QMainWindow, QWidget { background: #f4f7fb; color: #1f2937; font-size: 14px; }
        #loginPage { background: #f4f7fb; }
        #loginTitle { color: #12355b; font-size: 28px; font-weight: 700; }
        #loginSubtitle { color: #64748b; font-size: 15px; }
        #loginError { color: #b42318; min-height: 22px; }
        #sidebar { background: #12355b; }
        #brand { color: white; font-size: 20px; font-weight: 700; padding: 18px 12px; }
        QListWidget { border: none; background: transparent; color: #dce8f5; outline: none; }
        QListWidget::item { padding: 14px 16px; border-radius: 5px; }
        QListWidget::item:selected { background: #1e6a9e; color: white; }
        #pageTitle { color: #12355b; font-size: 24px; font-weight: 700; }
        #metricCard { background: white; border: 1px solid #e2e8f0; border-radius: 7px; }
        #metricTitle { color: #64748b; }
        #metricValue { color: #12355b; font-size: 26px; font-weight: 700; }
        QLineEdit, QComboBox, QDoubleSpinBox { background: white; border: 1px solid #cbd5e1;
            border-radius: 5px; padding: 8px; }
        QPushButton { background: #1769aa; color: white; border: none; border-radius: 5px;
            padding: 9px 16px; }
        QPushButton:hover { background: #125486; }
        QPushButton#danger { background: #c9372c; }
        QTableWidget { background: white; border: 1px solid #dbe3ec; gridline-color: #edf1f5; }
        QHeaderView::section { background: #eef4f9; color: #334155; padding: 9px; border: none; }
        QTableWidget::item { padding: 7px; }
    )"));
}

void AdminMainWindow::seedDemoData()
{
    stations_ = {{1, QStringLiteral("国贸充电站"), QStringLiteral("北京市朝阳区建国门外大街"), 1.50, 10, 7},
                 {2, QStringLiteral("望京充电站"), QStringLiteral("北京市朝阳区望京街道"), 1.68, 8, 5},
                 {3, QStringLiteral("中关村充电站"), QStringLiteral("北京市海淀区中关村大街"), 1.35, 12, 9}};
    chargers_ = {{1, 1, QStringLiteral("GM-01"), QStringLiteral("国贸充电站"), QStringLiteral("快充"), 60, QStringLiteral("空闲"), 120},
                 {2, 1, QStringLiteral("GM-02"), QStringLiteral("国贸充电站"), QStringLiteral("慢充"), 7, QStringLiteral("使用中"), 84},
                 {3, 1, QStringLiteral("GM-03"), QStringLiteral("国贸充电站"), QStringLiteral("快充"), 60, QStringLiteral("故障"), 66},
                 {4, 2, QStringLiteral("WJ-01"), QStringLiteral("望京充电站"), QStringLiteral("快充"), 120, QStringLiteral("空闲"), 98},
                 {5, 2, QStringLiteral("WJ-02"), QStringLiteral("望京充电站"), QStringLiteral("慢充"), 7, QStringLiteral("空闲"), 53},
                 {6, 3, QStringLiteral("ZGC-01"), QStringLiteral("中关村充电站"), QStringLiteral("快充"), 60, QStringLiteral("使用中"), 143}};
    users_ = {{1, QStringLiteral("138****8000"), QStringLiteral("李先生"), QStringLiteral("268.00"), QStringLiteral("正常")},
              {2, QStringLiteral("139****1221"), QStringLiteral("王女士"), QStringLiteral("92.50"), QStringLiteral("正常")},
              {3, QStringLiteral("186****4812"), QStringLiteral("赵先生"), QStringLiteral("0.00"), QStringLiteral("冻结")}};
    revenue_ = {{QStringLiteral("09-01"), QStringLiteral("320.00"), 26},
                {QStringLiteral("09-02"), QStringLiteral("286.50"), 22},
                {QStringLiteral("09-03"), QStringLiteral("412.00"), 31},
                {QStringLiteral("09-04"), QStringLiteral("368.00"), 28},
                {QStringLiteral("09-05"), QStringLiteral("495.50"), 36},
                {QStringLiteral("09-06"), QStringLiteral("438.00"), 34},
                {QStringLiteral("09-07"), QStringLiteral("526.00"), 41}};
}

void AdminMainWindow::buildLoginPage()
{
    loginPage_ = new LoginWidget(this);
    connect(loginPage_, &LoginWidget::loginRequested, this, &AdminMainWindow::handleLogin);
}

void AdminMainWindow::buildWorkspace()
{
    pages_ = new QStackedWidget(this);
    pages_->addWidget(loginPage_);

    auto* workspace = new QWidget;
    auto* layout = new QHBoxLayout(workspace);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* sidebar = new QWidget;
    sidebar->setObjectName(QStringLiteral("sidebar"));
    sidebar->setFixedWidth(230);
    auto* sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(12, 20, 12, 16);
    auto* brand = new QLabel(QStringLiteral("NCS 运营中心"));
    brand->setObjectName(QStringLiteral("brand"));
    navigation_ = new QListWidget;
    navigation_->addItems({QStringLiteral("运营总览"), QStringLiteral("充电站管理"),
                           QStringLiteral("充电桩管理"), QStringLiteral("用户管理"),
                           QStringLiteral("智能预测")});
    navigation_->setCurrentRow(0);
    sidebarLayout->addWidget(brand);
    sidebarLayout->addWidget(navigation_);
    sidebarLayout->addStretch();
    sidebarLayout->addWidget(new QLabel(QStringLiteral("管理员 · 演示环境")));

    workspacePages_ = new QStackedWidget(workspace);
    workspacePages_->addWidget(createDashboardPage());
    workspacePages_->addWidget(createStationsPage());
    workspacePages_->addWidget(createChargersPage());
    workspacePages_->addWidget(createUsersPage());
    workspacePages_->addWidget(createPredictionsPage());
    connect(navigation_, &QListWidget::currentRowChanged, this,
            [this](int row) { setPage(row); });

    layout->addWidget(sidebar);
    layout->addWidget(workspacePages_, 1);
    pages_->addWidget(workspace);
}

QWidget* AdminMainWindow::createMetricCard(const QString& title, const QString& value,
                                           const QString& detail)
{
    auto* card = new QWidget;
    card->setObjectName(QStringLiteral("metricCard"));
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(18, 16, 18, 16);
    auto* titleLabel = new QLabel(title);
    titleLabel->setObjectName(QStringLiteral("metricTitle"));
    auto* valueLabel = new QLabel(value);
    valueLabel->setObjectName(QStringLiteral("metricValue"));
    auto* detailLabel = new QLabel(detail);
    detailLabel->setStyleSheet(QStringLiteral("color:#64748b;"));
    layout->addWidget(titleLabel);
    layout->addWidget(valueLabel);
    layout->addWidget(detailLabel);
    return card;
}

QWidget* AdminMainWindow::createDashboardPage()
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(30, 26, 30, 26);
    layout->setSpacing(18);
    layout->addWidget(heading(QStringLiteral("运营总览")));
    auto* metrics = new QHBoxLayout;
    metrics->setSpacing(14);
    metrics->addWidget(createMetricCard(QStringLiteral("今日营收"), QStringLiteral("¥526.00"),
                                        QStringLiteral("较昨日 +12.4%")));
    metrics->addWidget(createMetricCard(QStringLiteral("本月营收"), QStringLiteral("¥2,846.50"),
                                        QStringLiteral("完成订单 218 笔")));
    metrics->addWidget(createMetricCard(QStringLiteral("在线电桩"), QStringLiteral("28 / 30"),
                                        QStringLiteral("在线率 93.3%")));
    metrics->addWidget(createMetricCard(QStringLiteral("注册用户"), QStringLiteral("1,286"),
                                        QStringLiteral("今日新增 24 人")));
    layout->addLayout(metrics);

    auto* body = new QHBoxLayout;
    auto* revenueBox = new QGroupBox(QStringLiteral("近 7 日营收与订单"));
    auto* revenueLayout = new QVBoxLayout(revenueBox);
    auto* revenueTable =
        makeTable({QStringLiteral("日期"), QStringLiteral("营收"), QStringLiteral("订单量")});
    for (const auto& point : revenue_) {
        const int row = revenueTable->rowCount();
        revenueTable->insertRow(row);
        revenueTable->setItem(row, 0, new QTableWidgetItem(point.date));
        revenueTable->setItem(row, 1, new QTableWidgetItem(QStringLiteral("¥%1").arg(point.revenue)));
        revenueTable->setItem(row, 2, new QTableWidgetItem(QString::number(point.orders)));
    }
    revenueLayout->addWidget(revenueTable);
    auto* statusBox = new QGroupBox(QStringLiteral("电桩状态"));
    auto* statusLayout = new QVBoxLayout(statusBox);
    statusLayout->addWidget(new QLabel(QStringLiteral("空闲    20    66.7%")));
    statusLayout->addWidget(new QLabel(QStringLiteral("使用中   8    26.6%")));
    statusLayout->addWidget(new QLabel(QStringLiteral("故障     2     6.7%")));
    statusLayout->addSpacing(12);
    statusLayout->addWidget(new QLabel(QStringLiteral("设备健康度  93.3%")));
    statusLayout->addStretch();
    body->addWidget(revenueBox, 2);
    body->addWidget(statusBox, 1);
    layout->addLayout(body, 1);
    return page;
}

QWidget* AdminMainWindow::createStationsPage()
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(30, 26, 30, 26);
    auto* toolbar = new QHBoxLayout;
    stationSearch_ = new QLineEdit;
    stationSearch_->setPlaceholderText(QStringLiteral("搜索站点名称或地址"));
    auto* search = new QPushButton(QStringLiteral("搜索"));
    auto* add = new QPushButton(QStringLiteral("新增充电站"));
    auto* remove = new QPushButton(QStringLiteral("删除选中"));
    remove->setObjectName(QStringLiteral("danger"));
    toolbar->addWidget(stationSearch_, 1);
    toolbar->addWidget(search);
    toolbar->addStretch();
    toolbar->addWidget(add);
    toolbar->addWidget(remove);
    stationTable_ = makeTable({QStringLiteral("编号"), QStringLiteral("站点名称"), QStringLiteral("地址"),
                               QStringLiteral("单价/度"), QStringLiteral("总桩数"), QStringLiteral("空闲桩")});
    layout->addWidget(heading(QStringLiteral("充电站管理")));
    layout->addLayout(toolbar);
    layout->addWidget(stationTable_, 1);
    connect(search, &QPushButton::clicked, this, &AdminMainWindow::refreshStations);
    connect(stationSearch_, &QLineEdit::returnPressed, this, &AdminMainWindow::refreshStations);
    connect(add, &QPushButton::clicked, this, &AdminMainWindow::addStation);
    connect(remove, &QPushButton::clicked, this, &AdminMainWindow::removeStation);
    fillStationTable();
    return page;
}

QWidget* AdminMainWindow::createChargersPage()
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(30, 26, 30, 26);
    auto* toolbar = new QHBoxLayout;
    chargerSearch_ = new QLineEdit;
    chargerSearch_->setPlaceholderText(QStringLiteral("搜索电桩编号或所属站点"));
    chargerStatus_ = new QComboBox;
    chargerStatus_->addItems({QStringLiteral("全部状态"), QStringLiteral("空闲"),
                              QStringLiteral("使用中"), QStringLiteral("故障")});
    auto* search = new QPushButton(QStringLiteral("筛选"));
    auto* status = new QPushButton(QStringLiteral("修改状态"));
    auto* restart = new QPushButton(QStringLiteral("远程重启"));
    toolbar->addWidget(chargerSearch_, 1);
    toolbar->addWidget(chargerStatus_);
    toolbar->addWidget(search);
    toolbar->addStretch();
    toolbar->addWidget(status);
    toolbar->addWidget(restart);
    chargerTable_ = makeTable({QStringLiteral("编号"), QStringLiteral("站点"), QStringLiteral("类型"),
                               QStringLiteral("功率"), QStringLiteral("状态"), QStringLiteral("累计次数")});
    layout->addWidget(heading(QStringLiteral("充电桩管理")));
    layout->addLayout(toolbar);
    layout->addWidget(chargerTable_, 1);
    connect(search, &QPushButton::clicked, this, &AdminMainWindow::refreshChargers);
    connect(status, &QPushButton::clicked, this, &AdminMainWindow::updateChargerStatus);
    connect(restart, &QPushButton::clicked, this, [this] {
        const int row = chargerTable_->currentRow();
        if (row < 0) {
            showApiError(QStringLiteral("请先选择一个电桩"));
            return;
        }
        QMessageBox::information(this, QStringLiteral("远程重启"),
                                 QStringLiteral("已向 %1 发送重启指令，设备将在 2 秒内恢复。")
                                     .arg(chargerTable_->item(row, 0)->text()));
    });
    fillChargerTable();
    return page;
}

QWidget* AdminMainWindow::createUsersPage()
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(30, 26, 30, 26);
    auto* toolbar = new QHBoxLayout;
    userSearch_ = new QLineEdit;
    userSearch_->setPlaceholderText(QStringLiteral("按手机号或昵称搜索"));
    auto* search = new QPushButton(QStringLiteral("搜索"));
    auto* toggle = new QPushButton(QStringLiteral("冻结/解冻"));
    toolbar->addWidget(userSearch_, 1);
    toolbar->addWidget(search);
    toolbar->addStretch();
    toolbar->addWidget(toggle);
    userTable_ = makeTable({QStringLiteral("编号"), QStringLiteral("手机号"), QStringLiteral("昵称"),
                            QStringLiteral("余额"), QStringLiteral("状态")});
    layout->addWidget(heading(QStringLiteral("用户管理")));
    layout->addLayout(toolbar);
    layout->addWidget(userTable_, 1);
    connect(search, &QPushButton::clicked, this, &AdminMainWindow::refreshUsers);
    connect(toggle, &QPushButton::clicked, this, &AdminMainWindow::toggleUserStatus);
    fillUserTable();
    return page;
}

QWidget* AdminMainWindow::createPredictionsPage()
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(30, 26, 30, 26);
    layout->addWidget(heading(QStringLiteral("智能负荷预测")));
    auto* hint = new QLabel(QStringLiteral("预测数据来自 /api/v1/admin/predictions，当前展示演示数据。"));
    hint->setStyleSheet(QStringLiteral("color:#64748b;"));
    layout->addWidget(hint);
    auto* table = makeTable({QStringLiteral("目标时段"), QStringLiteral("站点"), QStringLiteral("预测充电量"),
                             QStringLiteral("预测空闲桩"), QStringLiteral("峰值标记")});
    const QStringList times = {QStringLiteral("09-03 18:00"), QStringLiteral("09-03 19:00"),
                               QStringLiteral("09-03 20:00"), QStringLiteral("09-03 21:00")};
    for (int i = 0; i < times.size(); ++i) {
        table->insertRow(i);
        table->setItem(i, 0, new QTableWidgetItem(times.at(i)));
        table->setItem(i, 1, new QTableWidgetItem(i % 2 ? QStringLiteral("望京充电站") : QStringLiteral("国贸充电站")));
        table->setItem(i, 2, new QTableWidgetItem(QStringLiteral("%1.2 kWh").arg(48 + i * 7)));
        table->setItem(i, 3, new QTableWidgetItem(QString::number(3 + i)));
        table->setItem(i, 4, new QTableWidgetItem(i < 2 ? QStringLiteral("高峰") : QStringLiteral("平峰")));
    }
    layout->addWidget(table, 1);
    auto* run = new QPushButton(QStringLiteral("触发预测任务"));
    layout->addWidget(run, 0, Qt::AlignLeft);
    connect(run, &QPushButton::clicked, this, [this] {
        statusBar()->showMessage(QStringLiteral("预测任务已提交，完成后将刷新预测结果。"), 4000);
    });
    return page;
}

void AdminMainWindow::setPage(int index)
{
    if (workspacePages_ != nullptr && index >= 0 && index < workspacePages_->count())
        workspacePages_->setCurrentIndex(index);
}

void AdminMainWindow::handleLogin(const QString& username, const QString& password)
{
    loginPage_->setBusy(true);
    loginPage_->showError({});
    if (username == QStringLiteral("admin") && password == QStringLiteral("123456")) {
        openWorkspace();
        return;
    }
    loginPage_->setBusy(false);
    loginPage_->showError(QStringLiteral("账号或密码错误，请重试"));
}

void AdminMainWindow::openWorkspace()
{
    loginPage_->setBusy(false);
    pages_->setCurrentIndex(1);
    if (workspacePages_ != nullptr) workspacePages_->setCurrentIndex(0);
    statusBar()->showMessage(QStringLiteral("当前为演示模式：后端接口接入后将自动替换示例数据。"));
}

void AdminMainWindow::showApiError(const QString& message)
{
    QMessageBox::warning(this, QStringLiteral("操作提示"), message);
}

} // namespace ncs::admin
