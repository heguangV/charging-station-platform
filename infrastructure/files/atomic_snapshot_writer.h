#pragma once

#include <string>

namespace ncs::infrastructure::files {

class AtomicSnapshotWriter final {
public:
  explicit AtomicSnapshotWriter(std::string destinationPath)
      : destinationPath_(std::move(destinationPath)) {}
  bool write(const std::string &contents) const;
  const std::string &destinationPath() const { return destinationPath_; }

private:
  std::string destinationPath_;
};

} // namespace ncs::infrastructure::files
