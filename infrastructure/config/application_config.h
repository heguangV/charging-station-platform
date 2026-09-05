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
