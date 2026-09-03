#include "bottom_navigation.h"

#include <QHBoxLayout>
#include <QPainter>
#include <QPropertyAnimation>
#include <QSequentialAnimationGroup>
#include <QToolButton>

namespace ncs::user
{
namespace
{
QIcon iconFor(BottomNavigation::Item item, const QColor& color)
{
    QPixmap pixmap(24, 24); pixmap.fill(Qt::transparent);
    QPainter p(&pixmap); p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color, 1.9, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (item == BottomNavigation::Item::Home) {
        p.drawPolyline(QPolygonF{{4, 11}, {12, 4}, {20, 11}}); p.drawRoundedRect(QRectF(6, 10, 12, 10), 2, 2); p.drawLine(QPointF(10, 20), QPointF(10, 15));
    } else if (item == BottomNavigation::Item::Orders) {
        p.drawRoundedRect(QRectF(6, 3, 12, 18), 2, 2);
        p.drawLine(QPointF(9, 9), QPointF(15, 9)); p.drawLine(QPointF(9, 13), QPointF(15, 13)); p.drawLine(QPointF(9, 17), QPointF(13, 17));
    } else {
        p.drawEllipse(QRectF(8, 4, 8, 8)); p.drawArc(QRectF(5, 11, 14, 11), 195 * 16, 150 * 16);
    }
    return QIcon(pixmap);
}
} // namespace
BottomNavigation::BottomNavigation(QWidget* parent) : QWidget(parent)
{
    setStyleSheet(QStringLiteral("BottomNavigation{background:#FFFFFF;border-top:1px solid #DDEBE8;}"));
    auto* layout = new QHBoxLayout(this); layout->setContentsMargins(18, 5, 18, 6); layout->setSpacing(8);
    home_ = new QToolButton; orders_ = new QToolButton; profile_ = new QToolButton;
    home_->setText(QStringLiteral("首页")); orders_->setText(QStringLiteral("订单")); profile_->setText(QStringLiteral("我的"));
    for (QToolButton* button : {home_, orders_, profile_}) { button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon); button->setIconSize(QSize(22, 22)); button->setMinimumHeight(53); layout->addWidget(button, 1); }
    connect(home_, &QToolButton::clicked, this, &BottomNavigation::homeRequested);
    connect(orders_, &QToolButton::clicked, this, &BottomNavigation::ordersRequested);
    connect(profile_, &QToolButton::clicked, this, &BottomNavigation::profileRequested);
    setCurrent(Item::Home);
}
void BottomNavigation::setCurrent(Item item)
{
    const bool animate = hasCurrent_ && current_ != item;
    current_ = item;
    hasCurrent_ = true;
    updateButton(home_, Item::Home, item == Item::Home, animate && item == Item::Home);
    updateButton(orders_, Item::Orders, item == Item::Orders, animate && item == Item::Orders);
    updateButton(profile_, Item::Profile, item == Item::Profile, animate && item == Item::Profile);
}

void BottomNavigation::updateButton(QToolButton* button, Item item, bool selected, bool animate)
{
    const QColor color(selected ? QStringLiteral("#0F766E") : QStringLiteral("#667085")); button->setIcon(iconFor(item, color));
    button->setStyleSheet(QStringLiteral("QToolButton{color:%1;background:%2;border:0;border-radius:12px;font-size:11px;font-weight:%3;}QToolButton:hover{background:#EAF5F3;}").arg(color.name(), selected ? QStringLiteral("#E2F3F0") : QStringLiteral("transparent"), selected ? QStringLiteral("700") : QStringLiteral("500")));
    if (!animate) return;
    auto* pulse = new QSequentialAnimationGroup(button);
    auto* grow = new QPropertyAnimation(button, "iconSize", pulse);
    grow->setDuration(125); grow->setStartValue(QSize(22, 22)); grow->setEndValue(QSize(27, 27));
    grow->setEasingCurve(QEasingCurve::OutBack);
    auto* settle = new QPropertyAnimation(button, "iconSize", pulse);
    settle->setDuration(135); settle->setStartValue(QSize(27, 27)); settle->setEndValue(QSize(22, 22));
    settle->setEasingCurve(QEasingCurve::OutCubic);
    connect(pulse, &QSequentialAnimationGroup::finished, pulse, &QObject::deleteLater);
    pulse->start();
}
} // namespace ncs::user
