#pragma once

class QApplication;

namespace ncs::user
{

class AppTheme final
{
  public:
    static void apply(QApplication& application);
};

} // namespace ncs::user
