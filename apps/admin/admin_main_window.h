#pragma once

#include <QMainWindow>

namespace ncs::admin
{

class AdminMainWindow final : public QMainWindow
{
  public:
    explicit AdminMainWindow(QWidget* parent = nullptr);
};

} // namespace ncs::admin
