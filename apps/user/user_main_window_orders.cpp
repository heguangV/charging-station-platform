#include "user_main_window.h"

#include "net/user_api.h"
#include "ui/bottom_navigation.h"
#include "ui/review_dialog.h"

#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTimeZone>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

namespace ncs::user
{
namespace
{
QString statusColor(const QString& status)
{
    if (status == QStringLiteral("已完成"))
        return QStringLiteral("#087443");
    if (status == QStringLiteral("充电中"))
        return QStringLiteral("#886719");
    if (status == QStringLiteral("已预约"))
        return QStringLiteral("#23794E");
    return QStringLiteral("#607362");
}

void clearCards(QVBoxLayout* cards)
{
    while (QLayoutItem* item = cards->takeAt(0))
    {
        delete item->widget();
        delete item;
    }
}
} // namespace

QWidget* UserMainWindow::createOrdersPage()
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 8, 0, 0);
    auto* title = new QLabel(QStringLiteral("我的订单"));
    title->setStyleSheet(QStringLiteral("font-size:22px;font-weight:700;color:#243F30;"));
    auto* heading = new QHBoxLayout;
    heading->addWidget(title);
    auto* help = new QToolButton;
    help->setText(QStringLiteral("ⓘ"));
    help->setToolTip(QStringLiteral("点击订单卡中的“查看小票”可查看详细信息"));
    help->setStyleSheet(QStringLiteral(
        "QToolButton{color:#23794E;border:0;background:transparent;font-size:17px;padding:2px;}"));
    heading->addWidget(help);
    heading->addStretch();
    auto* refresh = new QPushButton(QStringLiteral("刷新"));
    refresh->setCursor(Qt::PointingHandCursor);
    refresh->setStyleSheet(
        QStringLiteral("QPushButton{background:#E4F0DC;color:#23794E;border:0;border-radius:9px;"
                       "font-size:13px;font-weight:700;padding:7px 12px;}"
                       "QPushButton:hover{background:#D2ECE5;}"));
    heading->addWidget(refresh);
    layout->addLayout(heading);
    ordersScroll_ = new QScrollArea;
    ordersScroll_->setWidgetResizable(true);
    ordersScroll_->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget;
    ordersCards_ = new QVBoxLayout(content);
    ordersCards_->setContentsMargins(0, 2, 0, 2);
    ordersCards_->setSpacing(10);
    ordersScroll_->setWidget(content);
    layout->addWidget(ordersScroll_, 1);
    ordersEmpty_ = new QLabel(QStringLiteral("⚡ 还没有充电足迹"));
    ordersEmpty_->setAlignment(Qt::AlignCenter);
    ordersEmpty_->setStyleSheet(
        QStringLiteral("padding:52px 18px;color:#52716C;font-size:15px;line-height:1.7;"));
    layout->addWidget(ordersEmpty_, 1);
    ordersRetryButton_ = new QPushButton(QStringLiteral("重新加载"));
    ordersRetryButton_->setObjectName(QStringLiteral("primaryButton"));
    ordersRetryButton_->setMinimumHeight(42);
    ordersRetryButton_->hide();
    layout->addWidget(ordersRetryButton_);
    connect(refresh, &QPushButton::clicked, this, &UserMainWindow::refreshOrders);
    connect(ordersRetryButton_, &QPushButton::clicked, this, &UserMainWindow::refreshOrders);
    connect(help, &QToolButton::clicked, this,
            [help] {
                QToolTip::showText(help->mapToGlobal(QPoint(0, help->height())), help->toolTip(),
                                   help);
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
    if (userApi_ && onlineSession_)
    {
        const int requestId = ++ordersRequestId_;
        ordersScroll_->hide();
        ordersEmpty_->setText(QStringLiteral("正在加载…"));
        ordersEmpty_->show();
        ordersRetryButton_->hide();
        userApi_->orders(
            1, 50,
            [this, requestId](ApiReply reply)
            {
                if (requestId != ordersRequestId_)
                    return;
                if (!reply.ok())
                {
                    ordersEmpty_->setText(QStringLiteral("加载失败"));
                    ordersRetryButton_->show();
                    notify(reply.message, true);
                    return;
                }
                orderRecords_.clear();
                for (const QJsonValue& value :
                     reply.data.toObject().value(QStringLiteral("items")).toArray())
                {
                    const QJsonObject item = value.toObject();
                    OrderSummary order;
                    order.orderNo = item.value(QStringLiteral("orderNo")).toString();
                    order.stationName = item.value(QStringLiteral("stationName")).toString();
                    order.chargerCode = item.value(QStringLiteral("chargerCode")).toString();
                    order.status = item.value(QStringLiteral("statusText")).toString();
                    order.energyMwh = item.value(QStringLiteral("energyMwh")).toInt();
                    order.amountCent = item.value(QStringLiteral("amountCent")).toInt();
                    const auto formatTime = [](const QJsonValue& timestamp)
                    {
                        if (timestamp.isNull() || timestamp.isUndefined())
                            return QString{};
                        return QDateTime::fromSecsSinceEpoch(timestamp.toVariant().toLongLong(),
                                                             QTimeZone::utc())
                            .toLocalTime()
                            .toString(QStringLiteral("yyyy-MM-dd HH:mm"));
                    };
                    order.startTime = formatTime(item.value(QStringLiteral("startedAt")));
                    order.endTime = formatTime(item.value(QStringLiteral("endedAt")));
                    if (!order.orderNo.isEmpty())
                        orderRecords_.append(std::move(order));
                }
                renderOrders(orderRecords_);
            });
        return;
    }
    renderOrders(service_.orders());
}

void UserMainWindow::renderOrders(const QVector<OrderSummary>& records)
{
    ordersScroll_->setVisible(!records.isEmpty());
    ordersEmpty_->setVisible(records.isEmpty());
    ordersRetryButton_->hide();
    if (records.isEmpty())
        ordersEmpty_->setText(QStringLiteral("⚡ 还没有充电足迹"));
    clearCards(ordersCards_);
    for (const OrderSummary& order : records)
    {
        auto* card = new QFrame;
        card->setObjectName(QStringLiteral("card"));
        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(15, 13, 15, 13);
        cardLayout->setSpacing(7);
        auto* header = new QHBoxLayout;
        auto* station = new QLabel(order.stationName);
        station->setStyleSheet(QStringLiteral("font-size:15px;font-weight:600;color:#243F30;"));
        auto* status = new QLabel(order.status);
        status->setStyleSheet(QStringLiteral("color:%1;background:#F3F8F7;border-radius:8px;"
                                             "padding:4px 7px;font-size:12px;font-weight:600;")
                                  .arg(statusColor(order.status)));
        header->addWidget(station);
        header->addStretch();
        header->addWidget(status);
        cardLayout->addLayout(header);
        auto* time = new QLabel(QStringLiteral("%1  ·  %2")
                                    .arg(order.chargerCode, order.startTime.isEmpty()
                                                                ? QStringLiteral("等待开始")
                                                                : order.startTime));
        time->setStyleSheet(QStringLiteral("font-size:12px;color:#607362;"));
        cardLayout->addWidget(time);
        auto* details = new QHBoxLayout;
        auto* energy = new QLabel(QStringLiteral("电量  %1 kWh")
                                      .arg(QString::number(order.energyMwh / 1000000.0, 'f', 3)));
        energy->setStyleSheet(QStringLiteral("font-size:13px;color:#475467;"));
        auto* amount = new QLabel(money(order.amountCent));
        amount->setStyleSheet(QStringLiteral("font-size:16px;font-weight:700;color:#23794E;"));
        QPushButton* reviewEntryRaw = nullptr;
        if (order.status == QStringLiteral("已完成"))
        {
            reviewEntryRaw = new QPushButton(QStringLiteral("写评价  ›"));
            reviewEntryRaw->setCursor(Qt::PointingHandCursor);
            reviewEntryRaw->setStyleSheet(
                QStringLiteral("QPushButton{background:transparent;color:#23794E;border:0;font-"
                               "size:12px;font-weight:600;padding:3px;}"
                               "QPushButton:hover{color:#174F34;}"));
            connect(reviewEntryRaw, &QPushButton::clicked, this,
                    [this, order, entry = QPointer<QPushButton>(reviewEntryRaw)]
                    { openReviewDialog(order.orderNo, order.stationName, entry); });
            details->addWidget(reviewEntryRaw);
        }
        const QPointer<QPushButton> reviewEntry(reviewEntryRaw);
        auto* receipt = new QPushButton(QStringLiteral("查看小票  ›"));
        receipt->setCursor(Qt::PointingHandCursor);
        receipt->setStyleSheet(
            QStringLiteral("QPushButton{background:transparent;color:#23794E;border:0;font-size:"
                           "12px;font-weight:600;padding:3px;}"
                           "QPushButton:hover{color:#174F34;}"));
        details->addWidget(energy);
        details->addWidget(amount);
        details->addStretch();
        details->addWidget(receipt);
        cardLayout->addLayout(details);
        connect(
            receipt, &QPushButton::clicked, this,
            [this, order, reviewEntry]
            {
                if (userApi_)
                {
                    userApi_->order(
                        order.orderNo,
                        [this, order, reviewEntry](ApiReply reply)
                        {
                            if (!reply.ok())
                            {
                                notify(reply.message, true);
                                return;
                            }
                            const QJsonObject value = reply.data.toObject();
                            const auto time = [](const QJsonValue& timestamp)
                            {
                                return QDateTime::fromSecsSinceEpoch(
                                           timestamp.toVariant().toLongLong(), QTimeZone::utc())
                                    .toLocalTime()
                                    .toString(QStringLiteral("yyyy-MM-dd HH:mm"));
                            };
                            QMessageBox receiptBox(
                                QMessageBox::Information, QStringLiteral("订单小票"),
                                QStringLiteral("订单号  %1\n电站  %2\n电桩  %3\n开始  %4\n结束  "
                                               "%5\n充电时长  %6 分钟\n电量  %7 kWh\n电费  %8 / "
                                               "度\n服务费  %9 / 度\n实付  %10\n状态  %11")
                                    .arg(
                                        value.value(QStringLiteral("orderNo")).toString(),
                                        value.value(QStringLiteral("stationName")).toString(),
                                        value.value(QStringLiteral("chargerCode")).toString(),
                                        time(value.value(QStringLiteral("startedAt"))),
                                        time(value.value(QStringLiteral("endedAt"))),
                                        QString::number(
                                            value.value(QStringLiteral("durationSec")).toInteger() /
                                            60),
                                        QString::number(
                                            value.value(QStringLiteral("energyMwh")).toInteger() /
                                                1000000.0,
                                            'f', 3),
                                        money(
                                            value
                                                .value(QStringLiteral("electricityPriceCentPerKwh"))
                                                .toInt()),
                                        money(value.value(QStringLiteral("servicePriceCentPerKwh"))
                                                  .toInt()),
                                        money(value.value(QStringLiteral("paidCent")).toInt()),
                                        value.value(QStringLiteral("statusText")).toString()),
                                QMessageBox::Ok, this);
                            QPushButton* writeReview = nullptr;
                            if (value.value(QStringLiteral("statusText")).toString() ==
                                QStringLiteral("已完成"))
                                writeReview = receiptBox.addButton(QStringLiteral("写评价"),
                                                                   QMessageBox::ActionRole);
                            receiptBox.exec();
                            if (writeReview && receiptBox.clickedButton() == writeReview)
                                openReviewDialog(order.orderNo, order.stationName, reviewEntry);
                        });
                    return;
                }
                QMessageBox receiptBox(
                    QMessageBox::Information, QStringLiteral("订单小票"),
                    QStringLiteral("订单号  %1\n电站  %2\n电桩  %3\n开始  %4\n结束  %5\n电量  %6 "
                                   "kWh\n金额  %7\n状态  %8")
                        .arg(order.orderNo, order.stationName, order.chargerCode,
                             order.startTime.isEmpty() ? QStringLiteral("--") : order.startTime,
                             order.endTime.isEmpty() ? QStringLiteral("--") : order.endTime,
                             QString::number(order.energyMwh / 1000000.0, 'f', 3),
                             money(order.amountCent), order.status),
                    QMessageBox::Ok, this);
                QPushButton* writeReview = nullptr;
                if (order.status == QStringLiteral("已完成"))
                    writeReview =
                        receiptBox.addButton(QStringLiteral("写评价"), QMessageBox::ActionRole);
                receiptBox.exec();
                if (writeReview && receiptBox.clickedButton() == writeReview)
                    openReviewDialog(order.orderNo, order.stationName, reviewEntry);
            });
        ordersCards_->addWidget(card);
    }
    ordersCards_->addStretch();
}

void UserMainWindow::openReviewDialog(const QString& orderNo, const QString& stationName,
                                      QPushButton* cardButton)
{
    const QPointer<QPushButton> button(cardButton);
    auto* dialog = new ReviewDialog(userApi_, &service_, orderNo, stationName, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &ReviewDialog::reviewed, this,
            [button]
            {
                if (button)
                    button->setText(QStringLiteral("查看评价  ›"));
            });
    dialog->exec();
}

} // namespace ncs::user
