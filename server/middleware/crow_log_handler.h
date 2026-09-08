// Crow 日志桥接：把 Crow 内部日志接入 StructuredLogger（统一 UTC 时间/级别/模块/请求 ID
// 格式），并做基础设施日志级别到 Crow 级别的映射。 出口对消息脱敏（如剔除 URL 查询串），避免 token
// 等敏感信息进入日志。
#pragma once

#include "infrastructure/files/structured_logger.h"

#include <crow.h>

#include <string>

namespace ncs::server::middleware
{

class CrowLogHandler final : public crow::ILogHandler
{
  public:
    explicit CrowLogHandler(ncs::infrastructure::files::StructuredLogger& logger);

    void log(const std::string& message, crow::LogLevel level) override;

  private:
    ncs::infrastructure::files::StructuredLogger& logger_;
};

crow::LogLevel toCrowLogLevel(ncs::infrastructure::files::LogLevel level);

} // namespace ncs::server::middleware
