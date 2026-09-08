// ML 模型产物（.pkl）文件存储：实现 core::application::ModelArtifactStore 接口，
// 负责产物路径管理（active/staging/按版本归档）、校验和核对与训练任务定稿（finalize）登记。
// 过期产物随引用它的预测记录一并清理；最新合格模型的产物始终保留，供 PREDICT 启动时重新校验。
#pragma once

#include "core/application/analytics_service.h"

#include <optional>
#include <string>

namespace ncs::infrastructure::files
{

class FileModelArtifactStore final : public core::application::ModelArtifactStore
{
  public:
    explicit FileModelArtifactStore(std::string activePath) : activePath_(std::move(activePath)) {}
    bool verify(std::string_view taskNo, std::string_view checksum) const override;
    bool verifyArtifact(std::string_view path, std::string_view checksum) const override;
    bool finalize(std::string_view taskNo, bool qualified,
                  std::string_view modelVersionNo) override;
    std::string activePath() const override
    {
        return activePath_;
    }
    std::string artifactPath(std::string_view modelVersionNo) const override;
    std::string stagingPath(std::string_view taskNo) const;
    // Retained artifacts expire together with the prediction rows that keep
    // their version rows referenced. The newest qualified model's artifact is
    // always kept so PREDICT launches can re-verify it.
    void cleanupExpired(const std::optional<std::string>& keepArtifactPath = std::nullopt);

  private:
    std::string activePath_;
};

} // namespace ncs::infrastructure::files
