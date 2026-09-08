#include "admin_main_window.h"
#include "admin_charts.h"
#include "admin_main_window_utils.h"
#include "admin_page_status.h"
#include "login_widget.h"
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
namespace ncs::admin
{
namespace
{
QIcon navIcon(int index)
{
    QPixmap pix(44, 44);
    pix.setDevicePixelRatio(2);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor("#4C7160"), 1.5));
    if (index == 0)
    {
        for (int i = 0; i < 4; ++i)
            p.drawRoundedRect(QRectF(3 + (i % 2) * 9, 3 + (i / 2) * 9, 6, 6), 1.5, 1.5);
    }
    else if (index == 1)
    {
        p.drawRoundedRect(QRectF(4, 3, 13, 16), 2, 2);
        p.drawLine(7, 7, 14, 7);
        p.drawLine(7, 11, 14, 11);
    }
    else if (index == 2)
    {
        p.drawRoundedRect(QRectF(5, 2, 11, 18), 2, 2);
        p.drawLine(12, 6, 9, 11);
        p.drawLine(9, 11, 13, 11);
        p.drawLine(13, 11, 10, 16);
    }
    else if (index == 3)
    {
        p.drawEllipse(QRectF(7, 3, 8, 8));
        p.drawArc(QRectF(3, 12, 16, 13), 0, 180 * 16);
    }
    else
    {
        p.drawLine(3, 18, 19, 18);
        p.drawLine(3, 18, 3, 3);
        p.drawPolyline(QPolygonF{{4, 14}, {9, 10}, {13, 12}, {18, 5}});
    }
    return QIcon(pix);
}
QPushButton* secondary(const QString& text)
{
    auto* b = new QPushButton(text);
    b->setObjectName("secondaryButton");
    return b;
}
} // namespace
AdminMainWindow::AdminMainWindow(AdminApiClient& api, const QString& environment, QWidget* parent)
    : QMainWindow(parent), api_(api)
{
    setWindowTitle(QStringLiteral("NCS 充电运营中心"));
    setMinimumSize(1040, 680);
    resize(1360, 880);
    styleApplication();
    loginPage_ = new LoginWidget;
    connect(loginPage_, &LoginWidget::loginRequested, this, &AdminMainWindow::handleLogin);
    buildWorkspace(environment);
    connect(&api_, &AdminApiClient::sessionExpired, this,
            [this] { returnToLogin(QStringLiteral("登录已过期，请重新登录")); });
    pages_->setCurrentIndex(0);
}
void AdminMainWindow::styleApplication()
{
    setStyleSheet(QStringLiteral(R"(
QWidget {font-family:"Noto Sans CJK SC","Microsoft YaHei",sans-serif;font-size:14px;color:#182B39;background:transparent;}
QMainWindow,QDialog {background:#F5F6F8;} QLabel {background:transparent;} QLabel#muted {color:#75828B;font-size:12px;}
QFrame#sidebar,QFrame#loginPanel {background:#FFFFFF;border:1px solid #E6EBEE;border-radius:20px;}
QFrame#loginPanel {border-radius:24px;} QLabel#brandMark {background:#EF8B3A;color:white;border-radius:8px;padding:8px 12px;font-weight:800;font-size:18px;}
QLabel#loginTitle {font-size:28px;font-weight:800;} QLabel#loginHeroTitle {font-size:34px;font-weight:800;}
QLabel#loginBrand {color:#23794E;font-size:16px;font-weight:700;} QLabel#loginError {color:#B42318;font-size:13px;}
QLabel#pageTitle {font-size:26px;font-weight:800;} QLabel#metricTitle {font-size:13px;color:#65717B;}
QLabel#metricValue {font-size:28px;font-weight:800;color:#123347;} QFrame#card {background:white;border:1px solid #E5EAEE;border-radius:18px;}
QListWidget {border:0;background:transparent;outline:none;font-size:14px;} QListWidget::item {padding:14px 10px;margin:3px 0;border-radius:12px;}
QListWidget::item:selected {background:#EDF5E7;color:#23794E;} QListWidget::item:hover {background:#F5F8F1;}
QLineEdit,QComboBox,QSpinBox,QDoubleSpinBox {background:#F7F8FA;border:1px solid #E2E7EA;border-radius:10px;padding:10px 12px;selection-background-color:#23794E;}
QLineEdit:focus,QComboBox:focus,QAbstractSpinBox:focus {border-color:#23794E;} QComboBox::down-arrow {image:url(:/admin/chevron.svg);width:12px;height:8px;} QComboBox::drop-down {border:0;width:22px;}
QPushButton {background:#123347;color:white;border:1px solid #123347;border-radius:11px;padding:9px 16px;font-weight:600;min-height:22px;}
QPushButton:hover {background:#20465B;} QPushButton:focus {border:1px solid #70AE62;} QPushButton:disabled {background:#EDF0F2;border-color:#EDF0F2;color:#9AA5AD;}
QPushButton#secondaryButton {background:white;color:#344653;border:1px solid #DCE4E7;} QPushButton#secondaryButton:hover {background:#F1F6EC;border-color:#A8BFA8;}
QPushButton#dangerButton {background:#FFF4EE;color:#AB432E;border-color:#F2D4C9;}
QTableWidget {background:white;alternate-background-color:#FAFBFC;border:1px solid #E5EAEE;border-radius:12px;selection-background-color:#ECF5E5;selection-color:#183328;outline:0;}
QTableWidget::item {padding:9px 8px;border-bottom:1px solid #F0F3F5;} QHeaderView::section {background:#F6F8F9;color:#6A7984;border:0;padding:12px 8px;font-size:12px;font-weight:600;}
QScrollArea {border:0;} QScrollBar:vertical {background:#F2F4F5;width:8px;margin:0;} QScrollBar::handle:vertical {background:#CBD6D3;border-radius:4px;min-height:24px;}
QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical {height:0;} QProgressBar {border:0;background:#E7EFE3;border-radius:2px;} QProgressBar::chunk {background:#70AE62;border-radius:2px;}
QStatusBar {color:#72808B;background:#F5F6F8;font-size:12px;} QDialogButtonBox QPushButton {min-width:72px;} QToolTip {background:#203E30;color:white;padding:6px;border:0;}
)"));
}
void AdminMainWindow::buildWorkspace(const QString& environment)
{
    pages_ = new QStackedWidget;
    pages_->addWidget(loginPage_);
    auto* workspace = new QWidget;
    auto* outer = new QHBoxLayout(workspace);
    outer->setContentsMargins(18, 18, 18, 8);
    outer->setSpacing(18);
    auto* sidebar = new QFrame;
    sidebar->setObjectName("sidebar");
    sidebar->setFixedWidth(190);
    auto* side = new QVBoxLayout(sidebar);
    side->setContentsMargins(14, 24, 14, 18);
    side->setSpacing(14);
    auto* logo = new QLabel(QStringLiteral("NCS"));
    logo->setObjectName("brandMark");
    side->addWidget(logo, 0, Qt::AlignLeft);
    auto* subtitle = new QLabel(QStringLiteral("充电运营中心"));
    subtitle->setObjectName("muted");
    side->addWidget(subtitle);
    side->addSpacing(18);
    navigation_ = new QListWidget;
    navigation_->setObjectName("adminNavigation");
    const QStringList names{QStringLiteral("运营总览"), QStringLiteral("充电站"),
                            QStringLiteral("充电桩"), QStringLiteral("用户管理"),
                            QStringLiteral("智能预测")};
    for (int i = 0; i < names.size(); ++i)
        navigation_->addItem(new QListWidgetItem(navIcon(i), names[i]));
    side->addWidget(navigation_, 1);
    const QHash<QString, QString> environments{{"development", QStringLiteral("开发环境")},
                                               {"test", QStringLiteral("测试环境")},
                                               {"acceptance", QStringLiteral("验收环境")},
                                               {"production", QStringLiteral("正式环境")}};
    auto* env = new QLabel(environments.value(environment, environment));
    env->setObjectName("muted");
    side->addWidget(env);
    auto* right = new QVBoxLayout;
    right->setSpacing(6);
    auto* tools = new QHBoxLayout;
    accountLabel_ = new QLabel;
    accountLabel_->setTextFormat(Qt::PlainText);
    accountLabel_->setObjectName("adminAccountLabel");
    tools->addWidget(accountLabel_, 1);
    auto* refresh = secondary(QStringLiteral("刷新"));
    refresh->setObjectName("refreshCurrentPage");
    auto* password = secondary(QStringLiteral("修改密码"));
    auto* exit = secondary(QStringLiteral("退出登录"));
    exit->setObjectName("adminLogoutButton");
    tools->addWidget(refresh);
    tools->addWidget(password);
    tools->addWidget(exit);
    right->addLayout(tools);
    operationMessage_ = new QLabel;
    operationMessage_->setTextFormat(Qt::PlainText);
    operationMessage_->setWordWrap(true);
    operationMessage_->setObjectName("operationMessage");
    operationMessage_->hide();
    right->addWidget(operationMessage_);
    workspacePages_ = new QStackedWidget;
    workspacePages_->setObjectName("adminWorkspacePages");
    workspacePages_->addWidget(createDashboardPage());
    workspacePages_->addWidget(createStationsPage());
    workspacePages_->addWidget(createChargersPage());
    workspacePages_->addWidget(createUsersPage());
    workspacePages_->addWidget(createPredictionsPage());
    right->addWidget(workspacePages_, 1);
    outer->addWidget(sidebar);
    outer->addLayout(right, 1);
    pages_->addWidget(workspace);
    pages_->setObjectName("adminRootPages");
    setCentralWidget(pages_);
    connect(navigation_, &QListWidget::currentRowChanged, this,
            [this](int row)
            {
                if (row < 0)
                    return;
                workspacePages_->setCurrentIndex(row);
                if (api_.hasSession())
                    refreshCurrentPage();
            });
    navigation_->setCurrentRow(0);
    connect(refresh, &QPushButton::clicked, this, &AdminMainWindow::refreshCurrentPage);
    connect(password, &QPushButton::clicked, this, &AdminMainWindow::changePassword);
    connect(exit, &QPushButton::clicked, this, &AdminMainWindow::logout);
    connectionLabel_ = new QLabel(QStringLiteral("未登录"));
    statusBar()->addWidget(connectionLabel_, 1);
    auto* clock = new QLabel;
    statusBar()->addPermanentWidget(clock);
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, clock,
            [clock]
            {
                clock->setText(
                    dateTimeText(QDateTime::currentSecsSinceEpoch(), "yyyy/MM/dd HH:mm:ss") +
                    QStringLiteral("  北京时间"));
            });
    timer->start(1000);
}
QWidget* AdminMainWindow::newPage(int index, const QString& title, const QString& subtitle,
                                  QVBoxLayout** output)
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(6, 14, 6, 8);
    layout->setSpacing(12);
    layout->addWidget(heading(title));
    auto* text = new QLabel(subtitle);
    text->setObjectName("muted");
    text->setWordWrap(true);
    layout->addWidget(text);
    states_[index] = new AdminPageStatus;
    states_[index]->setObjectName(QStringLiteral("pageStatus%1").arg(index));
    connect(states_[index], &AdminPageStatus::retryRequested, this,
            &AdminMainWindow::refreshCurrentPage);
    layout->addWidget(states_[index]);
    *output = layout;
    return page;
}
void AdminMainWindow::addPager(QVBoxLayout* layout, int page)
{
    auto* row = new QHBoxLayout;
    pageLabels_[page] = new QLabel;
    pageLabels_[page]->setObjectName("muted");
    previous_[page] = secondary(QStringLiteral("上一页"));
    next_[page] = secondary(QStringLiteral("下一页"));
    previous_[page]->setObjectName(QStringLiteral("previousPage%1").arg(page));
    next_[page]->setObjectName(QStringLiteral("nextPage%1").arg(page));
    row->addWidget(pageLabels_[page], 1);
    row->addWidget(previous_[page]);
    row->addWidget(next_[page]);
    layout->addLayout(row);
    connect(previous_[page], &QPushButton::clicked, this,
            [this, page]
            {
                if (pageNumbers_[page] > 1)
                    --pageNumbers_[page];
                refreshCurrentPage();
            });
    connect(next_[page], &QPushButton::clicked, this,
            [this, page]
            {
                ++pageNumbers_[page];
                refreshCurrentPage();
            });
    updatePager(page, 0);
}
void AdminMainWindow::updatePager(int page, int total)
{
    if (!pageLabels_[page])
        return;
    pageLabels_[page]->setText(
        QStringLiteral("共 %1 条 · 第 %2 页").arg(total).arg(pageNumbers_[page]));
    previous_[page]->setEnabled(pageNumbers_[page] > 1);
    next_[page]->setEnabled(pageNumbers_[page] * 50 < total);
}
void AdminMainWindow::refreshCurrentPage()
{
    if (!api_.hasSession() || passwordChangeRequired_)
        return;
    if (catalog_.isEmpty() && !catalogLoading_)
        loadCatalog();
    switch (workspacePages_->currentIndex())
    {
    case 0:
        refreshOverview();
        break;
    case 1:
        refreshStations();
        break;
    case 2:
        refreshChargers();
        break;
    case 3:
        refreshUsers();
        break;
    case 4:
        refreshPredictions();
        break;
    }
}
void AdminMainWindow::notify(const QString& text, bool error)
{
    operationMessage_->setText(text);
    operationMessage_->setStyleSheet(
        error ? "color:#B42318;padding:8px;background:#FFF1EB;border-radius:8px;"
              : "color:#23794E;padding:8px;background:#EEF5E7;border-radius:8px;");
    operationMessage_->setVisible(!text.isEmpty());
}
void AdminMainWindow::setBusy(bool busy)
{
    operationBusy_ = busy;
    for (auto* b : mutationButtons_)
        b->setEnabled(!busy);
}
qint64 AdminMainWindow::selectedId(QTableWidget* table) const
{
    auto* item = table->item(table->currentRow(), 0);
    return item ? item->data(Qt::UserRole).toLongLong() : 0;
}
} // namespace ncs::admin
