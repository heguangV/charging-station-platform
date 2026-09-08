// 应用配置值对象（用户端/管理端客户端共用）：按“进程环境变量 > .env 文件 > 开发默认值”
// 的优先级解析运行环境、服务端地址/端口、数据库路径、日志目录、TLS 证书、计费时间倍率、
// 模拟短信与 Dashboard 快照开关、腾讯地图 Key 等配置项，通过 load() 以 Result 返回只读实例。
// .env 中的服务端地图 Key（TENCENT_MAP_SERVER_KEY）会被拒绝读取，避免密钥进入客户端配置。
#pragma once

#include "ncs/core/result.h"

#include <QString>
#include <QStringList>

namespace ncs::infrastructure
{

class ApplicationConfig final
{
  public:
    static ncs::core::Result<ApplicationConfig> load(const QString& envFilePath = {});

    const QString& environment() const noexcept
    {
        return environment_;
    }
    const QString& serverHost() const noexcept
    {
        return serverHost_;
    }
    quint16 serverPort() const noexcept
    {
        return serverPort_;
    }
    const QString& databasePath() const noexcept
    {
        return databasePath_;
    }
    const QString& logDirectory() const noexcept
    {
        return logDirectory_;
    }
    const QString& tlsCertificatePath() const noexcept
    {
        return tlsCertificatePath_;
    }
    const QString& tlsPrivateKeyPath() const noexcept
    {
        return tlsPrivateKeyPath_;
    }
    bool allowInsecureHttp() const noexcept
    {
        return allowInsecureHttp_;
    }
    int billingTimeMultiplier() const noexcept
    {
        return billingTimeMultiplier_;
    }
    bool simulatedSmsEnabled() const noexcept
    {
        return simulatedSmsEnabled_;
    }
    bool dashboardSnapshotEnabled() const noexcept
    {
        return dashboardSnapshotEnabled_;
    }
    const QString& tencentMapWebKey() const noexcept
    {
        return tencentMapWebKey_;
    }
    const QString& tencentMapJsOrigin() const noexcept
    {
        return tencentMapJsOrigin_;
    }

    QStringList safeSummary() const;

  private:
    QString environment_;
    QString serverHost_;
    quint16 serverPort_ = 0;
    QString databasePath_;
    QString logDirectory_;
    QString tlsCertificatePath_;
    QString tlsPrivateKeyPath_;
    bool allowInsecureHttp_ = false;
    int billingTimeMultiplier_ = 1;
    bool simulatedSmsEnabled_ = false;
    bool dashboardSnapshotEnabled_ = true;
    QString tencentMapWebKey_;
    QString tencentMapJsOrigin_;
};

} // namespace ncs::infrastructure
