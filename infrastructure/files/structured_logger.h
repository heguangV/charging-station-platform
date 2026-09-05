#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace ncs::infrastructure::files {

enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error,
    Critical,
};

std::string_view logLevelName(LogLevel level);
std::string sanitizeSensitiveData(std::string_view message);

class RequestLogScope final {
public:
    explicit RequestLogScope(std::string requestId);
    ~RequestLogScope();

    RequestLogScope(const RequestLogScope &) = delete;
    RequestLogScope &operator=(const RequestLogScope &) = delete;

private:
    std::string previousRequestId_;
};

std::string_view currentRequestId();

class StructuredLogger final {
public:
    struct Options {
        std::string directory;
        LogLevel minimumLevel = LogLevel::Info;
        int retentionDays = 30;
        bool consoleEnabled = true;
    };

    explicit StructuredLogger(Options options);
    ~StructuredLogger();

    StructuredLogger(const StructuredLogger &) = delete;
    StructuredLogger &operator=(const StructuredLogger &) = delete;

    void log(
        LogLevel level,
        std::string_view module,
        std::string_view message,
        std::string_view requestId = {});
    void cleanupExpired();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ncs::infrastructure::files
