#pragma once

#include <QMainWindow>

namespace ncs::user
{

class UserMainWindow final : public QMainWindow
{
  public:
    explicit UserMainWindow(QWidget* parent = nullptr);
};

} // namespace ncs::user
