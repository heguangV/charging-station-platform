#include "charger_table.h"

#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace ncs::user
{
namespace
{
// 折叠态默认展示的电桩数量，其余通过“更多”展开。
constexpr int kCollapsedCount = 3;

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
        if (onDoubleClick_)
            onDoubleClick_();
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
    layout->setSpacing(9);
    auto* header = new QHBoxLayout;
    auto* heading = new QLabel(QStringLiteral("枪桩信息"));
    heading->setStyleSheet(QStringLiteral("font-size:16px;font-weight:700;color:#243F30;"));
    summaryLabel_ = new QLabel;
    summaryLabel_->setStyleSheet(QStringLiteral("font-size:13px;font-weight:700;color:#087443;"));
    header->addWidget(heading);
    header->addStretch();
    header->addWidget(summaryLabel_);
    layout->addLayout(header);
    selectedLabel_ = new QLabel;
    selectedLabel_->setWordWrap(true);
    selectedLabel_->setStyleSheet(QStringLiteral("font-size:12px;color:#23794E;"));
    selectedLabel_->hide();
    layout->addWidget(selectedLabel_);
    cards_ = new QVBoxLayout;
    cards_->setContentsMargins(0, 0, 0, 0);
    cards_->setSpacing(9);
    layout->addLayout(cards_);
    emptyLabel_ = new QLabel(QStringLiteral("该站点暂无电桩信息"));
    emptyLabel_->setAlignment(Qt::AlignCenter);
    emptyLabel_->setStyleSheet(QStringLiteral("padding:22px 12px;color:#98A2B3;font-size:13px;"));
    layout->addWidget(emptyLabel_);
    toggleButton_ = new QPushButton;
    toggleButton_->setCursor(Qt::PointingHandCursor);
    toggleButton_->setMinimumHeight(38);
    toggleButton_->setStyleSheet(
        QStringLiteral("QPushButton{background:#F0F6F1;color:#23794E;border:0;border-radius:10px;"
                       "font-size:13px;font-weight:700;}"
                       "QPushButton:hover{background:#E4F0DC;}"));
    connect(toggleButton_, &QPushButton::clicked, this,
            [this]
            {
                expanded_ = !expanded_;
                rebuild();
            });
    layout->addWidget(toggleButton_);
    toggleButton_->hide();
    emptyLabel_->hide();
}

void ChargerTable::setChargers(const QVector<ChargerSummary>& chargers)
{
    const bool sameStation = !chargers_.isEmpty() && !chargers.isEmpty() &&
                             chargers_.front().code == chargers.front().code;
    chargers_ = chargers;
    if (!sameStation)
        expanded_ = false;
    const bool stillAvailable = std::any_of(chargers_.cbegin(), chargers_.cend(),
                                            [this](const ChargerSummary& charger) {
                                                return charger.code == selectedCode_ &&
                                                       charger.status == QStringLiteral("空闲");
                                            });
    if (!stillAvailable)
        selectedCode_.clear();
    rebuild();
    emit selectionChanged();
}

void ChargerTable::rebuild()
{
    while (QLayoutItem* item = cards_->takeAt(0))
    {
        item->widget()->hide();
        item->widget()->deleteLater();
        delete item;
    }
    selectedLabel_->setText(QStringLiteral("已选：%1").arg(selectedCode_));
    selectedLabel_->setVisible(!selectedCode_.isEmpty());
    const int total = chargers_.size();
    const int idle = static_cast<int>(std::count_if(
        chargers_.cbegin(), chargers_.cend(),
        [](const ChargerSummary& charger) { return charger.status == QStringLiteral("空闲"); }));
    summaryLabel_->setText(QStringLiteral("空闲 %1/%2").arg(idle).arg(total));
    summaryLabel_->setStyleSheet(
        QStringLiteral("font-size:13px;font-weight:700;color:%1;")
            .arg(idle > 0 ? QStringLiteral("#087443") : QStringLiteral("#B42318")));
    emptyLabel_->setVisible(total == 0);
    const int shown = expanded_ ? total : qMin(kCollapsedCount, total);
    for (int index = 0; index < shown; ++index)
    {
        const ChargerSummary& charger = chargers_.at(index);
        const bool available = charger.status == QStringLiteral("空闲");
        const bool selected = selectedCode_ == charger.code;
        auto* card =
            new ChargerCard(available ? [this, code = charger.code] { toggleSelection(code); }
                                      : std::function<void()>{});
        card->setObjectName(QStringLiteral("chargerCard"));
        card->setCursor(available ? Qt::PointingHandCursor : Qt::ArrowCursor);
        card->setToolTip(available ? QStringLiteral("点击按钮或双击卡片，可选择或取消")
                                   : charger.status);
        card->setStyleSheet(
            QStringLiteral(
                "QFrame#chargerCard{background:%1;border:2px solid %2;border-radius:14px;}")
                .arg(selected ? QStringLiteral("#DDF5EF") : QStringLiteral("#FFFFFF"),
                     selected ? QStringLiteral("#23794E") : QStringLiteral("#DDEBE8")));
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
        text->setSpacing(6);
        auto* code = new QLabel(charger.code);
        code->setWordWrap(true);
        code->setMinimumWidth(0);
        code->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        code->setStyleSheet(QStringLiteral("font-size:16px;font-weight:700;color:#243F30;"));
        auto* meta =
            new QLabel(QStringLiteral("%1 · 累计 %2 次").arg(charger.type).arg(charger.totalCount));
        meta->setWordWrap(true);
        meta->setMinimumWidth(0);
        meta->setStyleSheet(QStringLiteral("font-size:12px;color:#607362;"));
        const QString color = available ? QStringLiteral("#087443")
                              : charger.status == QStringLiteral("故障")
                                  ? QStringLiteral("#B42318")
                                  : QStringLiteral("#886719");
        auto* status = new QLabel(selected ? QStringLiteral("✓ 已选择") : charger.status);
        status->setStyleSheet(
            QStringLiteral("color:%1;font-size:12px;font-weight:600;").arg(color));
        text->addWidget(code);
        text->addWidget(meta);
        text->addWidget(status);
        layout->addLayout(text, 1);
        auto* power = new QLabel(QStringLiteral("%1\nkW").arg(charger.powerKw));
        power->setAlignment(Qt::AlignCenter);
        power->setStyleSheet(QStringLiteral("font-size:20px;font-weight:700;color:#23794E;"));
        layout->addWidget(power);
        auto* choose = new QPushButton(
            available ? (selected ? QStringLiteral("取消选择") : QStringLiteral("选择"))
                      : charger.status);
        choose->setFixedWidth(84);
        choose->setMinimumHeight(38);
        choose->setAccessibleName(
            (selected ? QStringLiteral("取消选择 ") : QStringLiteral("选择 ")) + charger.code);
        choose->setEnabled(available);
        choose->setStyleSheet(
            QStringLiteral(
                "QPushButton{background:%1;color:%2;border:0;border-radius:9px;font-weight:600;}"
                "QPushButton:hover{background:%3;}"
                "QPushButton:disabled{background:#F2F4F7;color:#98A2B3;}")
                .arg(selected ? QStringLiteral("#23794E") : QStringLiteral("#E4F0DC"),
                     selected ? QStringLiteral("white") : QStringLiteral("#23794E"),
                     selected ? QStringLiteral("#B42318") : QStringLiteral("#CFF0E9")));
        layout->addWidget(choose);
        if (available)
        {
            connect(choose, &QPushButton::clicked, this,
                    [this, code = charger.code] { toggleSelection(code); });
        }
        cards_->addWidget(card);
    }
    const bool hasHidden = total > kCollapsedCount;
    toggleButton_->setVisible(hasHidden);
    if (hasHidden)
    {
        toggleButton_->setText(
            expanded_ ? QStringLiteral("收起 ⌃")
                      : QStringLiteral("展开更多 %1 个桩 ⌄").arg(total - kCollapsedCount));
        toggleButton_->setAccessibleName(toggleButton_->text());
    }
}

void ChargerTable::toggleSelection(const QString& code)
{
    selectedCode_ = selectedCode_ == code ? QString() : code;
    rebuild();
    emit selectionChanged();
}

QString ChargerTable::selectedChargerCode() const
{
    return selectedCode_;
}

qint64 ChargerTable::selectedChargerId() const
{
    for (const ChargerSummary& charger : chargers_)
        if (charger.code == selectedCode_)
            return charger.id;
    return 0;
}

int ChargerTable::selectedChargerType() const
{
    for (const ChargerSummary& charger : chargers_)
        if (charger.code == selectedCode_)
            return charger.typeValue;
    return 0;
}

} // namespace ncs::user
