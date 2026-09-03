#pragma once

#include <QWidget>

class QToolButton;

namespace ncs::user
{
class BottomNavigation final : public QWidget
{
    Q_OBJECT
  public:
    enum class Item { Home, Orders, Profile };
    explicit BottomNavigation(QWidget* parent = nullptr);
    void setCurrent(Item item);
  signals:
    void homeRequested();
    void ordersRequested();
    void profileRequested();
  private:
    void updateButton(QToolButton* button, Item item, bool selected, bool animate);
    QToolButton* home_ = nullptr;
    QToolButton* orders_ = nullptr;
    QToolButton* profile_ = nullptr;
    Item current_ = Item::Home;
    bool hasCurrent_ = false;
};
} // namespace ncs::user
