#include "charge_soc_gauge.h"

#include <QPainter>

namespace ncs::user
{

ChargeSocGauge::ChargeSocGauge(QWidget* parent) : QWidget(parent)
{
    setMinimumHeight(142);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void ChargeSocGauge::setValue(int value)
{
    value_ = qBound(0, value, 100);
    update();
}

void ChargeSocGauge::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const int diameter = qMin(width() - 28, height() - 18);
    const QRectF ring((width() - diameter) / 2.0, 6, diameter, diameter);
    QPen track(QColor(QStringLiteral("#E5ECF8")), 12, Qt::SolidLine, Qt::RoundCap);
    painter.setPen(track);
    painter.drawArc(ring, 225 * 16, -270 * 16);
    QPen progress(QColor(QStringLiteral("#19A974")), 12, Qt::SolidLine, Qt::RoundCap);
    painter.setPen(progress);
    painter.drawArc(ring, 225 * 16, -qRound(270.0 * value_ / 100.0) * 16);
    painter.setPen(QColor(QStringLiteral("#1D2939")));
    QFont valueFont = painter.font();
    valueFont.setPixelSize(28);
    valueFont.setBold(true);
    painter.setFont(valueFont);
    painter.drawText(ring, Qt::AlignCenter, QStringLiteral("%1%").arg(value_));
    QFont labelFont = painter.font();
    labelFont.setPixelSize(12);
    labelFont.setBold(false);
    painter.setFont(labelFont);
    painter.setPen(QColor(QStringLiteral("#667085")));
    painter.drawText(QRectF(0, ring.bottom() - 8, width(), 24), Qt::AlignHCenter,
                     QStringLiteral("电池 SoC"));
}

} // namespace ncs::user
