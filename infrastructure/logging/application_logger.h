// 客户端/应用日志入口：安装 Qt 消息处理器，把 qInfo/qWarning 等全局消息写入按日命名的
// 日志文件（ncs_yyyyMMdd.log），每行包含 UTC 时间、级别、模块、类别与请求 ID，并脱敏
// 敏感字段；初始化时清理超过 30 天的旧日志，setRequestId 在线程内关联当前请求 ID。
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
