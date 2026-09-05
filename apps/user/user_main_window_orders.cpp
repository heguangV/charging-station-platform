#include "user_main_window.h"

#include "ui/bottom_navigation.h"

#include <QAbstractItemView>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

namespace ncs::user
{

QWidget* UserMainWindow::createOrdersPage()
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 8, 0, 0);
    auto* title = new QLabel(QStringLiteral("我的订单"));
    title->setStyleSheet(QStringLiteral("font-size:23px;color:#25324A;"));
    auto* heading = new QHBoxLayout;
    heading->addWidget(title);
    auto* help = new QToolButton;
    help->setText(QStringLiteral("ⓘ"));
    help->setToolTip(QStringLiteral("双击任一订单可查看详细小票"));
    help->setStyleSheet(QStringLiteral("QToolButton{color:#0F766E;border:0;background:transparent;font-size:17px;padding:2px;}"));
    heading->addWidget(help);
    heading->addStretch();
    layout->addLayout(heading);
    ordersTable_ = new QTableWidget;
    ordersTable_->setColumnCount(4);
    ordersTable_->setHorizontalHeaderLabels({QStringLiteral("电站"), QStringLiteral("电桩"),
                                             QStringLiteral("金额"), QStringLiteral("状态")});
    ordersTable_->verticalHeader()->hide();
    ordersTable_->horizontalHeader()->setStretchLastSection(true);
    ordersTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ordersTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(ordersTable_, 1);
    ordersEmpty_ = new QLabel(QStringLiteral("⚡ 还没有充电足迹"));
    ordersEmpty_->setAlignment(Qt::AlignCenter);
    ordersEmpty_->setStyleSheet(QStringLiteral("padding:52px 18px;color:#52716C;font-size:15px;line-height:1.7;"));
    layout->addWidget(ordersEmpty_, 1);
    connect(help, &QToolButton::clicked, this, [help] {
        QToolTip::showText(help->mapToGlobal(QPoint(0, help->height())), help->toolTip(), help);
    });
    connect(ordersTable_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        const OrderSummary order = service_.orders().at(row);
        QMessageBox::information(
            this, QStringLiteral("订单小票"),
            QStringLiteral("订单号  %1\n电站  %2\n电桩  %3\n开始  %4\n结束  %5\n电量  %6 kWh\n金额  %7\n状态  %8")
                .arg(order.orderNo, order.stationName, order.chargerCode,
                     order.startTime.isEmpty() ? QStringLiteral("--") : order.startTime,
                     order.endTime.isEmpty() ? QStringLiteral("--") : order.endTime,
                     QString::number(order.energyMwh / 1000000.0, 'f', 3), money(order.amountCent),
                     order.status));
    });
    return page;
}

void UserMainWindow::showOrders()
{
    refreshOrders();
    bottomNavigation_->setCurrent(BottomNavigation::Item::Orders);
    bottomNavigation_->show();
    pages_->setCurrentIndex(6);
}

void UserMainWindow::refreshOrders()
{
    const QVector<OrderSummary> records = service_.orders();
    ordersTable_->setVisible(!records.isEmpty());
    ordersEmpty_->setVisible(records.isEmpty());
    ordersTable_->setRowCount(records.size());
    for (int row = 0; row < records.size(); ++row)
    {
        const OrderSummary& order = records.at(row);
        ordersTable_->setItem(row, 0, new QTableWidgetItem(order.stationName));
        ordersTable_->setItem(row, 1, new QTableWidgetItem(order.chargerCode));
        ordersTable_->setItem(row, 2, new QTableWidgetItem(money(order.amountCent)));
        auto* status = new QTableWidgetItem(order.status);
        if (order.status == QStringLiteral("已完成"))
        {
            status->setForeground(QColor(QStringLiteral("#087443")));
        }
        else if (order.status == QStringLiteral("充电中"))
        {
            status->setForeground(QColor(QStringLiteral("#B54708")));
        }
        else if (order.status == QStringLiteral("已预约"))
        {
            status->setForeground(QColor(QStringLiteral("#0F766E")));
        }
        else
        {
            status->setForeground(QColor(QStringLiteral("#667085")));
        }
        ordersTable_->setItem(row, 3, status);
    }
}

} // namespace ncs::user
