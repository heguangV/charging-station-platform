#pragma once

#include "core/application/analytics_service.h"

#include <optional>
#include <string>

namespace ncs::infrastructure::files {

class FileModelArtifactStore final
    : public core::application::ModelArtifactStore {
public:
  explicit FileModelArtifactStore(std::string activePath)
      : activePath_(std::move(activePath)) {}
  bool verify(std::string_view taskNo,
              std::string_view checksum) const override;
  bool verifyArtifact(std::string_view path,
                      std::string_view checksum) const override;
  bool finalize(std::string_view taskNo, bool qualified,
                std::string_view modelVersionNo) override;
  std::string activePath() const override { return activePath_; }
  std::string artifactPath(std::string_view modelVersionNo) const override;
  std::string stagingPath(std::string_view taskNo) const;
  // Retained artifacts expire together with the prediction rows that keep
  // their version rows referenced. The newest qualified model's artifact is
  // always kept so PREDICT launches can re-verify it.
  void cleanupExpired(const std::optional<std::string> &keepArtifactPath =
                          std::nullopt);

private:
  std::string activePath_;
};

} // namespace ncs::infrastructure::files
