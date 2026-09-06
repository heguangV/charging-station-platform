#pragma once
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QWidget>

namespace ncs::user
{
class EnergyScene final : public QWidget
{
  public:
    explicit EnergyScene(QWidget* parent = nullptr)
        : QWidget(parent), image_(QStringLiteral(":/ncs/resources/charging-scene.png"))
    {
        setFixedHeight(112);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setAccessibleName(QStringLiteral("安心充电，轻松出发"));
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
        QPainterPath clip;
        clip.addRoundedRect(QRectF(rect()), 16, 16);
        p.setClipPath(clip);
        p.fillRect(rect(), QColor("#FFF7E9"));
        if (!image_.isNull())
        {
            const qreal sourceHeight = image_.width() * height() / qreal(width());
            const qreal top = qMin(image_.height() * 0.28, image_.height() - sourceHeight);
            p.drawPixmap(QRectF(rect()), image_,
                         QRectF(0, qMax(0.0, top), image_.width(), sourceHeight));
        }
        QFont title = p.font();
        title.setPixelSize(20);
        title.setBold(true);
        p.setFont(title);
        p.setPen(QColor("#172C38"));
        p.drawText(QRectF(16, 19, width() / 2.0, 30), QStringLiteral("安心充电"));
        title.setPixelSize(13);
        title.setBold(false);
        p.setFont(title);
        p.setPen(QColor("#58636C"));
        p.drawText(QRectF(16, 53, width() / 2.0, 22), QStringLiteral("为下一程，蓄满能量"));
    }

  private:
    QPixmap image_;
};
} // namespace ncs::user
