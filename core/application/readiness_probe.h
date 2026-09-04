#pragma once

namespace ncs::core::application {

struct ReadinessStatus {
    bool schemaVersion = false;
    bool databaseReadWrite = false;
    bool walEnabled = false;
    bool migrationsComplete = false;

    bool ready() const
    {
        return schemaVersion && databaseReadWrite && walEnabled && migrationsComplete;
    }
};

class ReadinessProbe {
public:
    virtual ~ReadinessProbe() = default;
    virtual ReadinessStatus check() = 0;
};

class UnavailableReadinessProbe final : public ReadinessProbe {
public:
    ReadinessStatus check() override { return {}; }
};

} // namespace ncs::core::application
