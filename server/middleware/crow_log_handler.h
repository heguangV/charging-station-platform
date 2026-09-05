#pragma once

#include "infrastructure/files/structured_logger.h"

#include <crow.h>

#include <string>

namespace ncs::server::middleware {

class CrowLogHandler final : public crow::ILogHandler {
public:
    explicit CrowLogHandler(ncs::infrastructure::files::StructuredLogger &logger);

    void log(const std::string &message, crow::LogLevel level) override;

private:
    ncs::infrastructure::files::StructuredLogger &logger_;
};

crow::LogLevel toCrowLogLevel(ncs::infrastructure::files::LogLevel level);

} // namespace ncs::server::middleware
