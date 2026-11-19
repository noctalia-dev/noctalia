#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <gio/gio.h>
#include <optional>
#include <string>
#include <string_view>

namespace filemanager::checksum {
  enum class Algorithm { Sha256, Sha512, Md5 };
  struct Result {
    std::string hash;
    std::uint64_t bytes = 0;
  };
  struct ManifestEntry {
    Algorithm algorithm;
    std::string hash;
    std::string filename;
  };

  using Progress = std::function<void(std::uint64_t completed, std::uint64_t total)>;
  enum class Comparison { Match, Mismatch };

  [[nodiscard]] bool eligibleSelection(const std::filesystem::path& path, std::size_t count, bool localSelection);
  [[nodiscard]] std::string normalizeHash(std::string_view hash);
  // Call after validating expected against the selected algorithm.
  [[nodiscard]] Comparison compare(std::string_view calculated, std::string_view expected);
  [[nodiscard]] bool loadExpected(
      const std::filesystem::path& manifest, const std::filesystem::path& target, Algorithm algorithm,
      GCancellable* cancellable, std::string& expected, GError** error
  );
  [[nodiscard]] const char* name(Algorithm algorithm) noexcept;
  [[nodiscard]] std::optional<Algorithm> fromHash(std::string_view hash) noexcept;
  [[nodiscard]] std::optional<Algorithm> fromManifestName(std::string_view filename) noexcept;
  [[nodiscard]] bool validHash(std::string_view hash, Algorithm algorithm) noexcept;
  [[nodiscard]] std::optional<ManifestEntry> parseManifestLine(std::string_view line, Algorithm algorithm);
  [[nodiscard]] bool calculate(
      const std::filesystem::path& path, Algorithm algorithm, GCancellable* cancellable, Result& result, GError** error,
      const Progress& progress = {}
  );
} // namespace filemanager::checksum
