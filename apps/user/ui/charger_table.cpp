#include "charger_table.h"

#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <functional>

namespace ncs::user
{
namespace
{
class ChargerCard final : public QFrame
{
  public:
    explicit ChargerCard(std::function<void()> onDoubleClick, QWidget* parent = nullptr)
        : QFrame(parent), onDoubleClick_(std::move(onDoubleClick))
    {
    }

  protected:
    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
        if (onDoubleClick_) onDoubleClick_();
        event->accept();
    }

  private:
    std::function<void()> onDoubleClick_;
};
} // namespace

ChargerTable::ChargerTable(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget;
    cards_ = new QVBoxLayout(content);
    cards_->setContentsMargins(0, 0, 0, 0);
    cards_->setSpacing(9);
    scroll->setWidget(content);
    layout->addWidget(scroll);
    setMinimumHeight(245);
}

void ChargerTable::setChargers(const QVector<ChargerSummary>& chargers)
{
    chargers_ = chargers;
    selectedCode_.clear();
    rebuild();
}

void ChargerTable::rebuild()
{
    while (QLayoutItem* item = cards_->takeAt(0))
    {
        delete item->widget();
        delete item;
    }
    for (const ChargerSummary& charger : chargers_)
    {
        const bool available = charger.status == QStringLiteral("空闲");
        const bool selected = selectedCode_ == charger.code;
        auto* card = new ChargerCard(selected ? [this] {
            selectedCode_.clear();
            rebuild();
        } : std::function<void()>{});
        card->setCursor(available ? Qt::PointingHandCursor : Qt::ArrowCursor);
        card->setToolTip(selected ? QStringLiteral("已选择；双击卡片空白处或点击“取消选择”可取消")
                                  : QString());
        card->setStyleSheet(QStringLiteral("QFrame{background:%1;border:2px solid %2;border-radius:14px;}")
                                .arg(selected ? QStringLiteral("#DDF5EF") : QStringLiteral("#FFFFFF"),
                                     selected ? QStringLiteral("#0F766E") : QStringLiteral("#DDEBE8")));
        if (selected)
        {
            auto* shadow = new QGraphicsDropShadowEffect(card);
            shadow->setBlurRadius(18);
            shadow->setOffset(0, 4);
            shadow->setColor(QColor(15, 118, 110, 70));
            card->setGraphicsEffect(shadow);
        }
        auto* layout = new QHBoxLayout(card);
        layout->setContentsMargins(14, 11, 14, 11);
        auto* text = new QVBoxLayout;
        auto* code = new QLabel(charger.code);
        code->setStyleSheet(QStringLiteral("font-size:16px;font-weight:700;color:#25324A;"));
        auto* meta = new QLabel(QStringLiteral("%1 · 累计 %2 次").arg(charger.type).arg(charger.totalCount));
        meta->setStyleSheet(QStringLiteral("font-size:12px;color:#667085;"));
        const QString color = available ? QStringLiteral("#087443")
                                        : charger.status == QStringLiteral("故障") ? QStringLiteral("#B42318")
                                                                                : QStringLiteral("#B54708");
        auto* status = new QLabel(selected ? QStringLiteral("✓ 已选择 · 双击取消") : charger.status);
        status->setStyleSheet(QStringLiteral("color:%1;font-size:12px;font-weight:600;").arg(color));
        text->addWidget(code);
        text->addWidget(meta);
        text->addWidget(status);
        layout->addLayout(text, 1);
        auto* power = new QLabel(QStringLiteral("%1\nkW").arg(charger.powerKw));
        power->setAlignment(Qt::AlignCenter);
        power->setStyleSheet(QStringLiteral("font-size:20px;font-weight:700;color:#0F766E;"));
        layout->addWidget(power);
        auto* choose = new QPushButton(available ? (selected ? QStringLiteral("取消选择") : QStringLiteral("选择"))
                                                  : charger.status);
        choose->setMinimumSize(66, 36);
        choose->setEnabled(available);
        choose->setStyleSheet(QStringLiteral("QPushButton{background:%1;color:%2;border:0;border-radius:9px;font-weight:600;}"
                                              "QPushButton:hover{background:%3;}"
                                              "QPushButton:disabled{background:#F2F4F7;color:#98A2B3;}")
                                  .arg(selected ? QStringLiteral("#0F766E") : QStringLiteral("#E2F3F0"),
                                       selected ? QStringLiteral("white") : QStringLiteral("#0F766E"),
                                       selected ? QStringLiteral("#B42318") : QStringLiteral("#CFF0E9")));
        layout->addWidget(choose);
        if (available)
        {
            connect(choose, &QPushButton::clicked, this, [this, code = charger.code] {
                selectedCode_ = selectedCode_ == code ? QString() : code;
                rebuild();
            });
        }
        cards_->addWidget(card);
    }
    cards_->addStretch();
}

QString ChargerTable::selectedChargerCode() const
{
    return selectedCode_;
}

qint64 ChargerTable::selectedChargerId() const
{
    for (const ChargerSummary& charger : chargers_)
        if (charger.code == selectedCode_) return charger.id;
    return 0;
}

int ChargerTable::selectedChargerType() const
{
    for (const ChargerSummary& charger : chargers_)
        if (charger.code == selectedCode_) return charger.typeValue;
    return 0;
}

} // namespace ncs::user
