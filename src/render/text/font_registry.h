#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace text {

  // Register a font file with the process-global fontconfig config so its family
  // becomes available to Pango. Idempotent: registering the same path twice is a
  // no-op. Returns the resolved font family name, or an empty string on failure
  // (logged). Bumps fontConfigGeneration() on the first registration of a path.
  std::string registerFontFile(const std::filesystem::path& path);

  // Monotonically increasing counter bumped whenever a new font file is
  // registered. Text renderers compare against a cached value to know when to
  // re-read fontconfig, so a font loaded by any surface becomes visible to all.
  [[nodiscard]] std::uint64_t fontConfigGeneration();

} // namespace text
