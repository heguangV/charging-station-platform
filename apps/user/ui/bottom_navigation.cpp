#include "bottom_navigation.h"

#include <QHBoxLayout>
#include <QPainter>
#include <QToolButton>

namespace ncs::user
{
namespace
{
QIcon iconFor(BottomNavigation::Item item, const QColor& color)
{
    QPixmap pixmap(72, 72);
    pixmap.setDevicePixelRatio(3.0);
    pixmap.fill(Qt::transparent);
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
    setObjectName(QStringLiteral("bottomNavigation"));
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("QWidget#bottomNavigation{background:#FFFFFF;border:1px solid #E7EAEE;border-radius:18px;}"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(7, 7, 7, 7);
    layout->setSpacing(6);
    home_ = new QToolButton; orders_ = new QToolButton; profile_ = new QToolButton;
    home_->setText(QStringLiteral("首页")); orders_->setText(QStringLiteral("订单")); profile_->setText(QStringLiteral("我的"));
    for (QToolButton* button : {home_, orders_, profile_})
    {
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setIconSize(QSize(24, 24));
        button->setFixedHeight(56);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::StrongFocus);
        button->setCheckable(true);
        button->setAutoExclusive(true);
        button->setAccessibleName(button->text());
        layout->addWidget(button, 1);
    }
    connect(home_, &QToolButton::clicked, this, &BottomNavigation::homeRequested);
    connect(orders_, &QToolButton::clicked, this, &BottomNavigation::ordersRequested);
    connect(profile_, &QToolButton::clicked, this, &BottomNavigation::profileRequested);
    setCurrent(Item::Home);
}
void BottomNavigation::setCurrent(Item item)
{
    updateButton(home_, Item::Home, item == Item::Home);
    updateButton(orders_, Item::Orders, item == Item::Orders);
    updateButton(profile_, Item::Profile, item == Item::Profile);
}

void BottomNavigation::updateButton(QToolButton* button, Item item, bool selected)
{
    const QColor color(selected ? QStringLiteral("#0F766E") : QStringLiteral("#667085")); button->setIcon(iconFor(item, color));
    button->setChecked(selected);
    button->setStyleSheet(QStringLiteral(
        "QToolButton{color:%1;background:%2;border:1px solid transparent;"
        "border-radius:12px;font-size:12px;font-weight:%3;padding:3px 0;}"
        "QToolButton:hover{background:#EDF5F3;}"
        "QToolButton:pressed{background:#D8ECE6;}"
        "QToolButton:focus{border-color:#0F766E;}")
        .arg(color.name(), selected ? QStringLiteral("#E2F3EE") : QStringLiteral("transparent"),
             selected ? QStringLiteral("700") : QStringLiteral("500")));
}
} // namespace ncs::user
