#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

enum class AtomicWriteFallback {
  None,
  // Permit a checked in-place write to an existing regular file when replacement is unavailable.
  ExistingFile,
};

struct AtomicWriteTarget {
  std::filesystem::path path;
  bool throughSymlink = false;
};

std::optional<AtomicWriteTarget> resolveAtomicWriteTarget(const std::filesystem::path& path);
[[nodiscard]] bool writeTextFileAtomic(
    const std::filesystem::path& path, std::string_view content,
    std::optional<std::filesystem::perms> mode = std::nullopt, AtomicWriteFallback fallback = AtomicWriteFallback::None
);
