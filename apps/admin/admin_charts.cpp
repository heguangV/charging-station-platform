#include "admin_charts.h"
#include "admin_main_window_utils.h"
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <numeric>
namespace ncs::admin
{
AdminTrendWidget::AdminTrendWidget(QWidget* parent) : QWidget(parent)
{
    setMinimumHeight(190);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}
void AdminTrendWidget::setPoints(QList<RevenuePoint> points)
{
    points_ = std::move(points);
    update();
}
void AdminTrendWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF plot(54, 20, width() - 76, height() - 52);
    p.setPen(QColor("#E7EDEF"));
    for (int i = 0; i < 4; ++i)
    {
        double y = plot.top() + plot.height() * i / 3;
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }
    if (points_.isEmpty())
    {
        p.setPen(QColor("#65717B"));
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("暂无已结算订单"));
        return;
    }
    qint64 maxAmount = 100;
    for (const auto& v : points_)
        maxAmount = std::max(maxAmount, v.amountCent);
    p.setPen(QColor("#65717B"));
    for (int i = 0; i < 4; ++i)
    {
        double y = plot.top() + plot.height() * i / 3;
        p.drawText(QRectF(0, y - 8, 48, 18), Qt::AlignRight,
                   QString::number(maxAmount / 100.0 * (3 - i) / 3, 'f', 0));
    }
    QPainterPath line;
    const double step = plot.width() / std::max<qsizetype>(1, points_.size());
    for (qsizetype i = 0; i < points_.size(); ++i)
    {
        const double x = plot.left() + step * (i + .5),
                     y = plot.bottom() - plot.height() * points_[i].amountCent / maxAmount;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#E7F2DF"));
        p.drawRoundedRect(QRectF(x - step * .25, y, step * .5, plot.bottom() - y), 4, 4);
        if (i == 0)
            line.moveTo(x, y);
        else
            line.lineTo(x, y);
        if (points_.size() <= 7 || i % std::max<qsizetype>(1, points_.size() / 5) == 0)
        {
            p.setPen(QColor("#65717B"));
            p.drawText(QRectF(x - 28, plot.bottom() + 10, 56, 20), Qt::AlignCenter,
                       dateTimeText(points_[i].bucketStart, "MM/dd"));
        }
    }
    p.setPen(QPen(QColor("#23794E"), 2.5));
    p.setBrush(Qt::NoBrush);
    p.drawPath(line);
    for (qsizetype i = 0; i < points_.size(); ++i)
    {
        const double x = plot.left() + step * (i + .5),
                     y = plot.bottom() - plot.height() * points_[i].amountCent / maxAmount;
        p.setBrush(QColor("#23794E"));
        p.drawEllipse(QPointF(x, y), 3, 3);
    }
}
AdminStatusChart::AdminStatusChart(QWidget* parent) : QWidget(parent)
{
    setMinimumSize(300, 190);
}
void AdminStatusChart::setCounts(std::array<int, 5> counts)
{
    counts_ = counts;
    update();
}
void AdminStatusChart::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const std::array<QColor, 5> colors{{QColor("#70AE62"), QColor("#123347"), QColor("#E78566"),
                                        QColor("#E8C45C"), QColor("#BEC8CF")}};
    const QStringList labels{QStringLiteral("空闲"), QStringLiteral("使用中"),
                             QStringLiteral("故障"), QStringLiteral("重启中"),
                             QStringLiteral("已停用")};
    const int total = std::accumulate(counts_.begin(), counts_.end(), 0);
    const double size = std::min(140, height() - 24);
    const QRectF ring(10, (height() - size) / 2, size, size);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#EDF1F3"));
    p.drawEllipse(ring);
    double start = 90;
    for (int i = 0; i < 5; ++i)
    {
        const double angle = total > 0 ? counts_[i] * 360.0 / total : 0;
        p.setBrush(colors[i]);
        p.drawPie(ring, qRound(start * 16), qRound(-angle * 16));
        start -= angle;
    }
    p.setBrush(Qt::white);
    p.drawEllipse(ring.adjusted(15, 15, -15, -15));
    p.setPen(QColor("#123347"));
    QFont f = p.font();
    f.setPixelSize(26);
    f.setBold(true);
    p.setFont(f);
    p.drawText(ring, Qt::AlignCenter, QString::number(total));
    f.setPixelSize(12);
    f.setBold(false);
    p.setFont(f);
    const double x = ring.right() + 16;
    for (int i = 0; i < 5; ++i)
    {
        double y = 20 + i * 32;
        p.setPen(Qt::NoPen);
        p.setBrush(colors[i]);
        p.drawEllipse(QPointF(x + 4, y + 5), 4, 4);
        p.setPen(QColor("#425563"));
        const QString value = QStringLiteral("%1   %2  (%3%)")
                                  .arg(labels[i])
                                  .arg(counts_[i])
                                  .arg(total > 0 ? 100.0 * counts_[i] / total : 0, 0, 'f', 0);
        p.drawText(QRectF(x + 15, y - 4, width() - x - 15, 24), Qt::AlignVCenter, value);
    }
}
} // namespace ncs::admin
