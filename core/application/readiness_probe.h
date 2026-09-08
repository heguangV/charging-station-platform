// 服务就绪探针端口：检查 schema 版本、数据库读写、WAL 开启与迁移完成，全部通过 ready() 才算就绪。
// 供服务端启动自检/健康检查使用；UnavailableReadinessProbe 为恒不可用的兜底实现。
// 属端口接口，生产实现由基础设施层（SQLite 检查）提供。

#pragma once

namespace ncs::core::application
{

struct ReadinessStatus
{
    bool schemaVersion = false;
    bool databaseReadWrite = false;
    bool walEnabled = false;
    bool migrationsComplete = false;

    bool ready() const
    {
        return schemaVersion && databaseReadWrite && walEnabled && migrationsComplete;
    }
};

class ReadinessProbe
{
  public:
    virtual ~ReadinessProbe() = default;
    virtual ReadinessStatus check() = 0;
};

class UnavailableReadinessProbe final : public ReadinessProbe
{
  public:
    ReadinessStatus check() override
    {
        return {};
    }
};

} // namespace ncs::core::application
