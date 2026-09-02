#pragma once

#include "ncs/core/result.h"

#include <QString>

namespace ncs::infrastructure
{

class ApplicationLogger final
{
  public:
    static ncs::core::Result<void> initialize(const QString& logDirectory, const QString& module);
    static void setRequestId(const QString& requestId);
    static void shutdown();
};

} // namespace ncs::infrastructure
