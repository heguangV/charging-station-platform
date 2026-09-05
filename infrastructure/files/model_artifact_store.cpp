#include "infrastructure/files/model_artifact_store.h"

#include <openssl/evp.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>

namespace ncs::infrastructure::files {
namespace {

std::string sha256(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return {};
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
      EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
    return {};
  std::array<char, 64 * 1024> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    if (input.gcount() > 0 &&
        EVP_DigestUpdate(context.get(), buffer.data(),
                         static_cast<std::size_t>(input.gcount())) != 1)
      return {};
  }
  if (!input.eof())
    return {};
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int length = 0;
  if (EVP_DigestFinal_ex(context.get(), digest.data(), &length) != 1)
    return {};
  constexpr char hex[] = "0123456789abcdef";
  std::string value(length * 2, '\0');
  for (unsigned int index = 0; index < length; ++index) {
    value[index * 2] = hex[digest[index] >> 4];
    value[index * 2 + 1] = hex[digest[index] & 0x0f];
  }
  return value;
}

} // namespace

std::string FileModelArtifactStore::stagingPath(
    const std::string_view taskNo) const {
  return activePath_ + ".staging-" + std::string(taskNo);
}

std::string FileModelArtifactStore::artifactPath(
    const std::string_view modelVersionNo) const {
  const std::filesystem::path active(activePath_);
  return (active.parent_path() / (std::string(modelVersionNo) + ".pkl"))
      .string();
}

bool FileModelArtifactStore::verify(
    const std::string_view taskNo, const std::string_view checksum) const {
  return sha256(stagingPath(taskNo)) == checksum;
}

bool FileModelArtifactStore::verifyArtifact(
    const std::string_view path, const std::string_view checksum) const {
  const std::filesystem::path candidate(path);
  const std::filesystem::path allowedDirectory =
      std::filesystem::path(activePath_).parent_path();
  std::error_code error;
  const auto canonicalCandidate =
      std::filesystem::weakly_canonical(candidate, error);
  if (error)
    return false;
  const auto canonicalDirectory =
      std::filesystem::weakly_canonical(allowedDirectory, error);
  return !error && canonicalCandidate.parent_path() == canonicalDirectory &&
         sha256(canonicalCandidate) == checksum;
}

bool FileModelArtifactStore::finalize(
    const std::string_view taskNo, const bool qualified,
    const std::string_view modelVersionNo) {
  const std::filesystem::path staging(stagingPath(taskNo));
  std::error_code error;
  if (!qualified) {
    if (!std::filesystem::exists(staging, error))
      return true;
    std::filesystem::remove(staging, error);
    return !error;
  }
  const std::filesystem::path active(activePath_);
  std::filesystem::create_directories(active.parent_path(), error);
  if (error)
    return false;
  const std::filesystem::path retained(artifactPath(modelVersionNo));
  const std::filesystem::path retainedTemporary(retained.string() + ".tmp");
  std::filesystem::copy_file(staging, retainedTemporary,
                             std::filesystem::copy_options::overwrite_existing,
                             error);
  if (error)
    return false;
  std::filesystem::rename(retainedTemporary, retained, error);
  if (error)
    return false;
  std::filesystem::rename(staging, active, error);
  if (error) {
    std::filesystem::remove(retained, error);
    return false;
  }
  return true;
}

void FileModelArtifactStore::cleanupExpired(
    const std::optional<std::string> &keepArtifactPath) {
  const std::filesystem::path directory =
      std::filesystem::path(activePath_).parent_path();
  std::error_code error;
  if (!std::filesystem::is_directory(directory, error))
    return;
  // Retention matches the 90-day prediction cleanup: deleting an artifact
  // earlier would leave SUCCEEDED version rows pointing at missing files.
  const auto cutoff = std::filesystem::file_time_type::clock::now() -
                      std::chrono::hours(24 * 90);
  const std::string keepName =
      keepArtifactPath
          ? std::filesystem::path(*keepArtifactPath).filename().string()
          : std::string();
  for (const auto &entry : std::filesystem::directory_iterator(directory, error)) {
    if (error)
      return;
    const std::string name = entry.path().filename().string();
    if (name.rfind("MODEL", 0) != 0 || entry.path().extension() != ".pkl")
      continue;
    if (!keepName.empty() && name == keepName)
      continue;
    const auto modified = entry.last_write_time(error);
    if (!error && modified < cutoff)
      std::filesystem::remove(entry.path(), error);
    error.clear();
  }
}

} // namespace ncs::infrastructure::files
