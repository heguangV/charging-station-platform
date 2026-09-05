#include "infrastructure/files/atomic_snapshot_writer.h"

#include "core/application/security_crypto.h"

#include <filesystem>
#include <fstream>

namespace ncs::infrastructure::files {

bool AtomicSnapshotWriter::write(const std::string &contents) const {
  const std::filesystem::path destination(destinationPath_);
  const std::filesystem::path directory = destination.parent_path();
  const std::filesystem::path temporary =
      directory / (destination.filename().string() + ".tmp-" +
                   core::application::secureRandomToken(12));
  try {
    if (!directory.empty())
      std::filesystem::create_directories(directory);
    {
      std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
      if (!output)
        return false;
      output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
      output.flush();
      if (!output)
        throw std::runtime_error("snapshot write failed");
    }
    std::error_code permissionError;
    std::filesystem::permissions(
        temporary,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, permissionError);
#ifndef _WIN32
    if (permissionError)
      throw std::runtime_error("snapshot permission update failed");
#endif
    std::filesystem::rename(temporary, destination);
    return true;
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return false;
  }
}

} // namespace ncs::infrastructure::files
