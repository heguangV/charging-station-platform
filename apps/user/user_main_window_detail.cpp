#include "user_main_window.h"

#include "net/user_api.h"
#include "ui/charger_table.h"

#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace ncs::user
{
namespace
{
QLabel* label(const QString& value, int size = 14)
{
    auto* result = new QLabel(value);
    result->setWordWrap(true);
    result->setStyleSheet(QStringLiteral("font-size:%1px;color:#243F30;font-weight:%2;")
                              .arg(size)
                              .arg(size >= 20 ? 700 : 400));
    return result;
}

QFrame* card()
{
    auto* result = new QFrame;
    result->setObjectName(QStringLiteral("card"));
    return result;
}

const QString kReviewEmptyText = QStringLiteral("暂无评论 · 完成充电后可在「我的订单」中评价");
} // namespace

QWidget* UserMainWindow::createDetailPage()
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 8, 0, 0);
    layout->setSpacing(10);
    // 顶部标题栏：返回 + 居中页面标题；电站名称在下方信息卡中继续完整展示。
    auto* header = new QHBoxLayout;
    header->setSpacing(8);
    auto* back = button(QStringLiteral("‹ 返回"),
                        QStringLiteral("QPushButton{color:#23794E;border:0;background:transparent;"
                                       "text-align:left;font-size:14px;padding:0;}"));
    back->setFixedSize(84, 32);
    auto* pageTitle = label(QStringLiteral("场站详情"), 17);
    pageTitle->setAlignment(Qt::AlignCenter);
    pageTitle->setStyleSheet(QStringLiteral("font-size:17px;color:#243F30;font-weight:700;"));
    auto* headerBalance = new QWidget;
    headerBalance->setFixedWidth(84);
    header->addWidget(back);
    header->addStretch();
    header->addWidget(pageTitle);
    header->addStretch();
    header->addWidget(headerBalance);
    layout->addLayout(header);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget;
    auto* body = new QVBoxLayout(content);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(10);
    // 站点信息卡：名称、状态与距离、地址 + 导航入口。
    auto* infoCard = card();
    auto* infoLayout = new QVBoxLayout(infoCard);
    infoLayout->setContentsMargins(16, 14, 16, 14);
    infoLayout->setSpacing(8);
    detailTitle_ = label({}, 20);
    detailTitle_->setStyleSheet(QStringLiteral("font-size:20px;color:#243F30;font-weight:700;"));
    detailMeta_ = label({}, 13);
    detailMeta_->setStyleSheet(QStringLiteral("font-size:13px;color:#607362;"));
    auto* addressRow = new QHBoxLayout;
    addressRow->setSpacing(8);
    detailAddress_ = label({}, 13);
    detailAddress_->setStyleSheet(QStringLiteral("font-size:13px;color:#475467;"));
    auto* navigateBadge =
        button(QStringLiteral("导航"),
               QStringLiteral("QPushButton{background:#E4F0DC;color:#23794E;border:0;border-"
                              "radius:9px;font-size:13px;font-weight:700;padding:0 14px;}"));
    navigateBadge->setFixedHeight(32);
    addressRow->addWidget(detailAddress_, 1);
    addressRow->addWidget(navigateBadge);
    infoLayout->addWidget(detailTitle_);
    infoLayout->addWidget(detailMeta_);
    infoLayout->addLayout(addressRow);
    body->addWidget(infoCard);
    // 充电费强调卡：暖色底 + 大字号单价，突出电费与服务费构成。
    auto* feeCard = new QFrame;
    feeCard->setObjectName(QStringLiteral("feeCard"));
    feeCard->setStyleSheet(QStringLiteral("QFrame#feeCard{background:#FFF7EC;border:1px solid "
                                          "#FFE0B8;border-radius:18px;}"));
    auto* feeLayout = new QVBoxLayout(feeCard);
    feeLayout->setContentsMargins(16, 14, 16, 14);
    feeLayout->setSpacing(10);
    auto* feeHeader = new QHBoxLayout;
    auto* feeTitle = label(QStringLiteral("⚡ 充电费"), 17);
    feeTitle->setStyleSheet(QStringLiteral("font-size:17px;color:#1F2A37;font-weight:700;"));
    auto* feeHint = label(QStringLiteral("电费 + 服务费"), 12);
    feeHint->setStyleSheet(QStringLiteral("font-size:12px;color:#B0824A;"));
    feeHeader->addWidget(feeTitle);
    feeHeader->addStretch();
    feeHeader->addWidget(feeHint);
    feeLayout->addLayout(feeHeader);
    auto* priceRow = new QHBoxLayout;
    priceRow->setSpacing(6);
    auto* priceColumn = new QVBoxLayout;
    priceColumn->setSpacing(2);
    auto* priceCaption = label(QStringLiteral("当前单价（元/度）"), 12);
    priceCaption->setStyleSheet(QStringLiteral("font-size:12px;color:#B0824A;"));
    auto* priceValueRow = new QHBoxLayout;
    priceValueRow->setSpacing(5);
    feePriceLabel_ = label({}, 34);
    feePriceLabel_->setStyleSheet(QStringLiteral("font-size:34px;color:#D95E00;font-weight:800;"));
    auto* priceUnit = label(QStringLiteral("元/度"), 13);
    priceUnit->setStyleSheet(QStringLiteral("font-size:13px;color:#8A6A3B;padding-top:16px;"));
    priceValueRow->addWidget(feePriceLabel_);
    priceValueRow->addWidget(priceUnit);
    priceValueRow->addStretch();
    priceColumn->addWidget(priceCaption);
    priceColumn->addLayout(priceValueRow);
    priceRow->addLayout(priceColumn, 1);
    auto* priceTag = label(QStringLiteral("当前参考价"), 12);
    priceTag->setAlignment(Qt::AlignCenter);
    priceTag->setStyleSheet(QStringLiteral("font-size:12px;color:#9A5B13;background:#FFEDD3;"
                                           "border-radius:9px;padding:5px 10px;font-weight:600;"));
    priceRow->addWidget(priceTag, 0, Qt::AlignTop);
    feeLayout->addLayout(priceRow);
    feeBreakdownRow_ = new QWidget;
    auto* breakdown = new QHBoxLayout(feeBreakdownRow_);
    breakdown->setContentsMargins(0, 0, 0, 0);
    breakdown->setSpacing(10);
    const auto priceBox = [](const QString& caption, QLabel*& value)
    {
        auto* box = new QFrame;
        box->setObjectName(QStringLiteral("feeBox"));
        // 选择器限定 objectName：QLabel 继承 QFrame，宽泛的 QFrame{} 会给内部标签加上边框。
        box->setStyleSheet(QStringLiteral("QFrame#feeBox{background:#FFFBF3;border:1px solid "
                                          "#F3DDBB;border-radius:10px;}"));
        auto* boxLayout = new QVBoxLayout(box);
        boxLayout->setContentsMargins(12, 9, 12, 9);
        boxLayout->setSpacing(3);
        auto* captionLabel = label(caption, 12);
        captionLabel->setStyleSheet(QStringLiteral("font-size:12px;color:#B0824A;"));
        value = label({}, 15);
        value->setStyleSheet(QStringLiteral("font-size:15px;color:#1F2A37;font-weight:700;"));
        boxLayout->addWidget(captionLabel);
        boxLayout->addWidget(value);
        return box;
    };
    breakdown->addWidget(priceBox(QStringLiteral("电费"), feeElectricLabel_), 1);
    breakdown->addWidget(priceBox(QStringLiteral("服务费"), feeServiceLabel_), 1);
    feeBreakdownRow_->setVisible(false);
    feeLayout->addWidget(feeBreakdownRow_);
    auto* quoteHint = label(QStringLiteral("最终单价以预约后确认的报价为准"), 12);
    quoteHint->setStyleSheet(QStringLiteral("font-size:12px;color:#9A7041;"));
    feeLayout->addWidget(quoteHint);
    body->addWidget(feeCard);
    // 枪桩信息卡：ChargerTable 内置空闲统计与“更多”折叠。
    auto* chargerCard = card();
    auto* chargerLayout = new QVBoxLayout(chargerCard);
    chargerLayout->setContentsMargins(16, 14, 16, 14);
    chargerTable_ = new ChargerTable;
    chargerLayout->addWidget(chargerTable_);
    body->addWidget(chargerCard);
    // 车友评论卡：默认展示前两条，其余收纳进“更多”。
    auto* reviewCard = card();
    auto* reviewLayout = new QVBoxLayout(reviewCard);
    reviewLayout->setContentsMargins(16, 14, 16, 14);
    reviewLayout->setSpacing(11);
    auto* reviewHeader = new QHBoxLayout;
    auto* reviewTitle = label(QStringLiteral("车友评论"), 16);
    reviewTitle->setStyleSheet(QStringLiteral("font-size:16px;color:#243F30;font-weight:700;"));
    reviewCountLabel_ = label({}, 12);
    reviewCountLabel_->setStyleSheet(QStringLiteral("font-size:12px;color:#7B828A;"));
    reviewHeader->addWidget(reviewTitle);
    reviewHeader->addStretch();
    reviewHeader->addWidget(reviewCountLabel_);
    reviewLayout->addLayout(reviewHeader);
    reviewCards_ = new QVBoxLayout;
    reviewCards_->setContentsMargins(0, 0, 0, 0);
    reviewCards_->setSpacing(10);
    reviewLayout->addLayout(reviewCards_);
    reviewEmptyLabel_ = label(kReviewEmptyText, 13);
    reviewEmptyLabel_->setStyleSheet(QStringLiteral("font-size:13px;color:#98A2B3;"));
    reviewLayout->addWidget(reviewEmptyLabel_);
    reviewMoreButton_ = new QPushButton;
    reviewMoreButton_->setCursor(Qt::PointingHandCursor);
    reviewMoreButton_->setMinimumHeight(38);
    reviewMoreButton_->setStyleSheet(
        QStringLiteral("QPushButton{background:#F0F6F1;color:#23794E;border:0;border-radius:10px;"
                       "font-size:13px;font-weight:700;}"
                       "QPushButton:hover{background:#E4F0DC;}"));
    reviewMoreButton_->hide();
    reviewLayout->addWidget(reviewMoreButton_);
    connect(reviewMoreButton_, &QPushButton::clicked, this,
            [this]
            {
                reviewsExpanded_ = !reviewsExpanded_;
                rebuildReviewCards();
            });
    body->addWidget(reviewCard);
    body->addStretch();
    scroll->setWidget(content);
    layout->addWidget(scroll, 1);
    auto* reserve = button(QStringLiteral("请选择空闲电桩"));
    reserve->setEnabled(false);
    connect(chargerTable_, &ChargerTable::selectionChanged, this,
            [this, reserve]
            {
                const bool selected = !chargerTable_->selectedChargerCode().isEmpty();
                reserve->setEnabled(selected);
                reserve->setText(selected ? QStringLiteral("预约所选电桩")
                                          : QStringLiteral("请选择空闲电桩"));
                reserve->setToolTip(chargerTable_->selectedChargerCode());
            });
    auto* navigate = button(QStringLiteral("一键导航"));
    navigate->setObjectName(QStringLiteral("secondaryButton"));
    auto* actions = new QHBoxLayout;
    actions->setSpacing(10);
    actions->addWidget(navigate, 1);
    actions->addWidget(reserve, 2);
    layout->addLayout(actions);
    connect(back, &QPushButton::clicked, this, &UserMainWindow::showHome);
    connect(navigate, &QPushButton::clicked, this, &UserMainWindow::showNavigation);
    connect(navigateBadge, &QPushButton::clicked, this, &UserMainWindow::showNavigation);
    connect(reserve, &QPushButton::clicked, this,
            [this]
            {
                if (userApi_)
                {
                    beginFlowRequest();
                    return;
                }
                if (service_.hasUnfinishedOrder())
                {
                    QMessageBox::information(this, QStringLiteral("未完成订单"),
                                             QStringLiteral("您有未完成的充电订单，请先结算。"));
                    showCharge();
                    return;
                }
                QString message;
                selectedChargerCode_ = chargerTable_->selectedChargerCode();
                if (service_.reserve(selectedStationId_, selectedChargerCode_, &message))
                {
                    chargingStarted_ = false;
                    startButton_->setEnabled(true);
                    cancelButton_->setEnabled(true);
                    settleButton_->setEnabled(false);
                    chargeState_->setText(QStringLiteral("已预约 · %1").arg(selectedChargerCode_));
                    notify(message);
                    showCharge();
                }
                else
                {
                    notify(message, true);
                }
            });
    return page;
}

void UserMainWindow::updateFeeCard(const StationSummary& station)
{
    feePriceLabel_->setText(QString::number(station.priceCentPerKwh / 100.0, 'f', 2));
    const bool hasBreakdown =
        station.electricityPriceCentPerKwh >= 0 && station.servicePriceCentPerKwh >= 0 &&
        station.electricityPriceCentPerKwh + station.servicePriceCentPerKwh > 0;
    feeBreakdownRow_->setVisible(hasBreakdown);
    if (!hasBreakdown)
        return;
    feeElectricLabel_->setText(
        QStringLiteral("%1 元/度")
            .arg(QString::number(station.electricityPriceCentPerKwh / 100.0, 'f', 2)));
    feeServiceLabel_->setText(
        QStringLiteral("%1 元/度")
            .arg(QString::number(station.servicePriceCentPerKwh / 100.0, 'f', 2)));
}

void UserMainWindow::renderStationReviews(int stationId)
{
    reviewsExpanded_ = false;
    stationReviews_.clear();
    if (!userApi_)
    {
        stationReviews_ = service_.stationReviews(stationId);
        rebuildReviewCards();
        return;
    }
    // 在线模式拉取场站评论墙（UC-U-12）：展示加载、空态和失败状态，过期的回复直接丢弃。
    rebuildReviewCards();
    reviewCountLabel_->setText(QStringLiteral("加载中…"));
    reviewEmptyLabel_->setText(QStringLiteral("正在加载评论…"));
    reviewEmptyLabel_->setVisible(true);
    userApi_->stationReviews(
        stationId,
        [this, stationId](ApiReply reply)
        {
            if (stationId != selectedStationId_)
                return;
            if (!reply.ok())
            {
                rebuildReviewCards();
                reviewCountLabel_->setText(QStringLiteral("加载失败"));
                reviewEmptyLabel_->setText(QStringLiteral("评论加载失败，重新进入本页可重试"));
                reviewEmptyLabel_->setVisible(true);
                notify(reply.message, true);
                return;
            }
            QVector<StationReview> reviews;
            for (const QJsonValue& value :
                 reply.data.toObject().value(QStringLiteral("items")).toArray())
            {
                const QJsonObject item = value.toObject();
                reviews.append(
                    {item.value(QStringLiteral("author")).toString(),
                     item.value(QStringLiteral("rating")).toInt(),
                     item.value(QStringLiteral("content")).toString(),
                     QDateTime::fromSecsSinceEpoch(
                         item.value(QStringLiteral("createdAt")).toVariant().toLongLong())
                         .date()
                         .toString(QStringLiteral("MM-dd"))});
            }
            stationReviews_ = std::move(reviews);
            reviewsExpanded_ = false;
            rebuildReviewCards();
        });
}

void UserMainWindow::rebuildReviewCards()
{
    while (QLayoutItem* item = reviewCards_->takeAt(0))
    {
        delete item->widget();
        delete item;
    }
    const int total = stationReviews_.size();
    reviewCountLabel_->setText(total > 0 ? QStringLiteral("共 %1 条").arg(total)
                                         : QStringLiteral("暂无"));
    reviewEmptyLabel_->setText(kReviewEmptyText);
    reviewEmptyLabel_->setVisible(total == 0);
    // 折叠态默认展示两条，其余收纳进“更多”。
    const int shown = reviewsExpanded_ ? total : qMin(2, total);
    const QVector<QString> avatarColors = {QStringLiteral("#23794E"), QStringLiteral("#167C55"),
                                           QStringLiteral("#2F6F8F"), QStringLiteral("#B25E09"),
                                           QStringLiteral("#8A6A3B")};
    for (int index = 0; index < shown; ++index)
    {
        const StationReview& review = stationReviews_.at(index);
        auto* row = new QFrame;
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(10);
        auto* avatar = new QLabel(review.author.left(1));
        avatar->setFixedSize(30, 30);
        avatar->setAlignment(Qt::AlignCenter);
        avatar->setStyleSheet(
            QStringLiteral("background:%1;color:white;border-radius:15px;font-size:13px;"
                           "font-weight:700;")
                .arg(avatarColors.at(index % avatarColors.size())));
        rowLayout->addWidget(avatar);
        auto* text = new QVBoxLayout;
        text->setSpacing(3);
        auto* line = new QHBoxLayout;
        line->setSpacing(6);
        auto* name = new QLabel(review.author);
        name->setStyleSheet(QStringLiteral("font-size:13px;color:#344054;font-weight:600;"));
        auto* stars = new QLabel(QString(5, QChar(0x2605)).left(review.rating) +
                                 QString(5 - review.rating, QChar(0x2606)));
        stars->setStyleSheet(QStringLiteral("font-size:12px;color:#F59E0B;"));
        auto* time = new QLabel(review.time);
        time->setStyleSheet(QStringLiteral("font-size:11px;color:#98A2B3;"));
        line->addWidget(name);
        line->addWidget(stars);
        line->addStretch();
        line->addWidget(time);
        auto* contentLabel = new QLabel(review.content);
        contentLabel->setWordWrap(true);
        contentLabel->setStyleSheet(QStringLiteral("font-size:13px;color:#475467;"));
        text->addLayout(line);
        text->addWidget(contentLabel);
        rowLayout->addLayout(text, 1);
        reviewCards_->addWidget(row);
    }
    const bool hasHidden = total > 2;
    reviewMoreButton_->setVisible(hasHidden);
    if (hasHidden)
    {
        reviewMoreButton_->setText(reviewsExpanded_
                                       ? QStringLiteral("收起 ⌃")
                                       : QStringLiteral("展开更多 %1 条评论 ⌄").arg(total - 2));
        reviewMoreButton_->setAccessibleName(reviewMoreButton_->text());
    }
}

} // namespace ncs::user
